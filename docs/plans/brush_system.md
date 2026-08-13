# Programmable Brushes — the Brush Lab (SceneEditor)

**Stages 1, 2 and 3 implemented.** The Brush Lab tab, the Lua brush model, material roles bound to
palette entries, a preview that runs a brush and throws the result away, all three kinds running --
scatter included -- and the **Custom tool** (Ctrl+U), which runs the same brushes on the document with
one history entry per stroke. What is *not* built yet is stated in [Build order](#build-order): the
five native sculpt brushes are still an enum rather than registry entries, and scatter bakes voxels
rather than placing instances.

Read this for why the pieces are shaped the way they are. The built behaviour is documented in
[`docs/examples/SceneEditor/README.md`](../examples/SceneEditor/README.md) under *The Brush Lab*;
where the two differ, the README is what the code does.

## Goal

A brush the user writes. Four cases drove the design, and they do not divide the way "what is it for"
would suggest:

- **A procedural rock texture.** Needs *skin depth* (0 on the surface, 1 one voxel in) so cracks break
  the skin rather than running through the interior, needs a position that is coherent across the whole
  `.data`, and needs to be able to apply to the stone and leave the timber alone.
- **A grass brush.** Two jobs: tint the ground green, and plant blades and flowers standing up out of
  it. Those are two different brushes composed, not one brush with a flag.
- **A paint brush driven by the model.** Darker in the creases — which is a question about local solid
  density, not about noise.
- **A tree brush.** Places whole objects, each checked for room before it lands.

## The three kinds

Kinds are decided by **what the brush writes and which cells it is offered**, because that is the only
distinction the machinery actually cares about:

| Kind | Offered | Returns | Covers |
|---|---|---|---|
| **Material** | solid voxels only | a material index, or nil | the rock texture, the crease shading, the grass tint |
| **Geometry** | every cell of the dab | an index (fill), `false` (empty), or nil | cracks that open, blades as raw voxels |
| **Scatter** | sites on a lattice | a list of voxels, gated by a fit test | grass tufts, trees |

A rock texture and a grass tint are the *same kind*: both recolour what exists and neither can create
a voxel. That is worth more than a taxonomy organised by intent, because the kind is exactly what
decides whether an empty cell is a question the brush gets asked.

Material and Geometry are one signature with two write paths. Scatter is genuinely different, and
building it proved the split was right: its domain is a sparse point set rather than a volume, its
answer is a shape rather than one cell's fate, and its cost is "how many did I plant" rather than "how
many cells did I visit". It shares the context, the parameters, the palette roles and the journal --
and nothing else. One interface serving all three would have been the wrong shape.

Its site selection is the part worth reading twice, because it is where three wants meet: plants that
do not clump, a second pass that plants nothing new, and the same ground giving the same plants every
time. Sites come from a **lattice fixed to the model**, not from the dab -- the dab only decides which
cells are visited, never which voxel within a cell wins -- and all three fall out of that one choice.
See *Scatter: sites, not cells* in the README.

The tick-versus-stamp distinction (`sculptBrushIsIterative`) is *not* a fourth kind. It is a declared
flag, the way Smooth and Bump already work.

## The other central decision: the palette owns materials, the brush owns roles

A brush declares **roles** — "the crack colour", "the fresh face", "what the user wants painted" — and
each role points at an entry in the component's palette. It does not own a colour, cannot edit one, and
has no picker anywhere in its interface.

That falls out of a single rule: **there is one place a material is created, recoloured, renamed and
removed, and it is the Palette panel.** The Brush Lab shows that panel — the same function drawing the
same state, not a second palette that agrees — so the entry selected in the Lab is the entry selected
in the Edit tab.

It sits at the foot of the Lab's right-hand column, which is where the Edit tab docks it. It was
briefly a full-width strip along the bottom, on the reasoning that the palette belongs to the component
rather than to the brush and so should not live in the brush's column. That reasoning is true and
irrelevant: a panel that is one width and place in one tab and a different width and place in the other
is a panel the user has to re-find, and the palette is the one surface the two tabs share.

Three consequences worth stating, because each is a thing that would otherwise go wrong:

- **Declared colours are creation defaults.** They are consulted on exactly one occasion: no entry of
  that name exists and one has to be made. After that the palette's copy wins. Without that rule,
  running a brush over one more patch would silently undo a colour the user had just set by hand —
  the script would be a second source of truth, asserting itself once per stroke.
- **Bindings are stored by name, not by slot index.** A slot index is a fact about one component's
  palette and means something else in the next one, and a palette removal renumbers every slot above
  it. A name is what `internMaterial` already matches on first, so a binding made while working on the
  castle still means the right thing on the terrain.
- **A colour parameter is a palette reference, not a picker**, and it is a *material role* — it goes in
  the same expanded list, counts against the same palette ceiling, and gets an index. That index is
  what makes it useful rather than decorative: a brush returns a material index, so without
  `p.tint.index` a script could read the colour the user chose and have no way to paint with it.

The gesture is the same everywhere: click the role, click the entry. It is parked as an intent
(`brushBindingRole`) and resolved where the brush is known, the way `PickPurpose` already works — the
palette body knows only that *something* is waiting for an entry, and nothing about brushes.

## The central decision: a brush returns an index, not a colour

This one is forced, not chosen, and everything else about the output follows from it.

A component's palette holds 255 entries (`MAX_MATERIALS_PER_COMPONENT`; material IDs are `uint8_t`).
The edit queue carries *colours*, and `applyComponentQueue` interns whatever colour it is handed. So a
brush returning free-form RGB spends a palette entry per distinct value — a few thousand voxels of a
noise texture exhausts the palette, and `internMaterial` then fails loudly having painted nothing. The
error message in `material.cpp` already says the answer out loud: *"quantize or reduce the color set."*

So a brush **declares the materials it can write**, the editor resolves them against the palette once
when a stroke begins, and the brush returns one by index. Three things fall out, all of them wanted:

- the palette cost of a brush is known before it runs, and is its declared count;
- a brush can set glossiness, metallic and emission per voxel — a returned colour could not;
- entries are matched **by name**, so re-running the same brush tomorrow lands on the same entries
  rather than adding a second set beside them. Which is also what makes a binding portable; see above.

A ramp is the same declaration with the entries generated: `steps` colours interpolated between two,
named `<name>.1 .. <name>.N`. A crack that darkens with depth wants sixteen greys and should not have
to write them out. The declared ceiling is 128 entries; past that a brush is storing an image, and the
answer is fewer steps.

## Context is declared, not queried

The expensive context is skin depth and crevice occupancy, and both are questions about a voxel's
*neighbours*. Asked of the tree64 one cell at a time that is ~28 descents per cell per field — the
same finding that made Smooth unaffordable before `SculptScratch` existed.

So a brush lists what it reads (`needs = { "position", "skinDepth", ... }`) and the editor computes
those and nothing else, once per dab, over a dense box:

- **`skinDepth`** — breadth-first from every exposed face inward, giving exact 6-connected distance
  from the surface. A two-pass chamfer would be cheaper and would answer a different question
  (Chebyshev distance counts a diagonal as one step, reporting a corner voxel as shallower than it is);
  for "how far has this crack bitten", steps through the material are the honest measure.
- **`crevice`** — the solid fraction of a ball, computed on demand for the dab's own cells rather than
  for the whole box.
- **`normal`** — the gradient of the local solid mask: points out of the material, roughly unit length
  on a surface voxel, near zero deep inside. Which is the right behaviour for "is this face up".
- **`position`** — the component's own voxel lattice, and this is the default deliberately. Lattice
  space is coherent across the whole `.data` and across separate strokes; world space re-textures the
  object every time it is moved.

The declaration is what buys the margin. The box is the dab plus `max(maxSkinDepth + 1, creviceRadius,
1)` on every side, so a brush that does not ask for skin depth does not pay for it, and one that asks
for 6 pays for 6 rather than the worst case. Nothing is read within the margin of the boundary, so no
answer inside the dab depends on what is outside the box. Outside the box reads as **solid**, which is
the safe direction — an empty answer there would invent a surface at the boundary and report every deep
voxel near it as skin.

## Purity is a rule, not a style

Same context, same parameters, same answer. No state between calls, no wall clock (`os` is removed from
the sandbox outright, not trimmed to `os.time`). Hash-based randomness, never a stateful RNG.

Three things depend on it, and each of them breaks visibly without it: a stroke that crosses its own
path churns; a mirrored cell disagrees with its original; and a preview stops being a preview of
anything. The seed parameter's Roll button steps by a fixed amount rather than randomising, for the
same reason — a sequence of arrangements has to be walkable backwards.

## The editor keeps the writing

A brush never touches the scene. It is handed a context and returns a verdict; the editor gathers the
verdicts and writes them. That is what keeps the four things a plugin API would quietly break:

1. **The journal.** A cell the preview has already *changed* is skipped before the script is called, so
   it can never be changed twice — which is what makes the revert exact however many dabs overlap. A
   cell the brush *declined* is not journalled and is asked again, deliberately: remembering every cell
   ever considered would put the whole swept volume into the structure the revert walks, and a carving
   brush's answer legitimately changes once material has gone.
2. **Symmetry.** Mirrored cells go through the same journal, on the way in — see the comment above
   `stampSculptDab`, which is emphatic about why mirroring downstream of the journal is wrong.
3. **The budget.** One dab is capped at 120k cells; the cost of a radius is cubic in it.
4. **One queue per dab.** `updateScene` forks and rebuilds every chunk a queue touches, so a thousand
   voxels queued together cost one rebuild each and a thousand separate calls would cost a thousand.

## Why Lua, and why it is fast enough

Lua 5.4.7, pinned as a submodule in *this example's* `external/`, compiled straight into the editor
like ImGui. Compiled with `LUA_USE_POSIX` rather than `LUA_USE_LINUX` on purpose: the Linux preset adds
`LUA_USE_DLOPEN`, and the one capability the sandbox cannot take back is the one not compiled in.

The sandbox removes `dofile`, `loadfile`, `load`, `require`, `package`, `io`, `debug`,
`collectgarbage` and `os`. It is a guard against accidents and against a brush quietly ceasing to be
reproducible — not a security boundary, and the doc says so rather than implying otherwise.

**Per-voxel scripting is viable because the noise is native.** `pv.worley`, `pv.noise`, `pv.fbm`,
`pv.hash` and the interpolation helpers are C++; the script composes them. Measured on the shipped
brushes: 85–385 ns per voxel, 2.6–11.8 M calls/second. A radius-8 dab is ~2000 voxels; the 49³ ceiling
a committed stroke will want is ~23 ms at the slowest brush measured. The expensive part of a
procedural texture was never the composition.

The runaway guard is **time per call, checked from a count hook** — 250 ms. A pure instruction budget
was the first attempt and is wrong: Lua's count hook fires on a counter that runs across the whole
state rather than per call, so a stroke of a hundred thousand honest thirty-instruction calls trips a
two-million-instruction budget exactly as reliably as one infinite loop does, and kills the stroke at a
random voxel. This matters because a script is saved and reloaded every few seconds while it is being
written, so `while true do end` is a normal event.

## Why the Lab is a tab

A brush is a file in the user's brushes folder, shared by every scene they open. The Edit tab's subject
is a document; the Lab's subject is a tool that outlives every document. And designing one is a
four-surface loop — library, declaration, settings, output — that has nothing to say while a scene is
being arranged and that six existing panels have nowhere to put.

It costs nothing on the renderer: it borrows Edit's, so switching allocates no targets and the preview
is the same stored-albedo view the Viewport shows — which is the only honest view of what a brush
actually wrote.

Three columns behind two splitters, not a dockspace: the four surfaces are always all wanted and always
in the same relationship, so the freedom to rearrange them buys nothing and costs a second layout in
`imgui.ini` to keep migrated.

## Settings: declared in the script, or added in the editor

Both, and the script cannot tell the difference — it reads them out of the same table. That is what the
requirement has to mean, or it is two features wearing one name.

The schema comes from the script's `params` list. Anything added in the Lab goes in a
`<brush>.params.json` sidecar beside the file, along with the *values* of both sets. Two files rather
than one because they have different authors: the `.lua` belongs to whoever wrote the brush and to
version control beside it, while the values are the local user's dial settings and change every time a
slider moves. Writing settings back into the script would mean an editor that rewrites the author's
source on every drag.

A reload carries values across, matched on name *and* type. Without that the Lab is unusable: a script
is saved every few seconds while a number is being tuned, and a reload that reset every slider would
undo the tuning being done.

Eight types, each of which is a widget the panel can draw, a value the sidecar can store, and a Lua
value the script receives — three things that have to agree, so a type is not free. `Seed` is separate
from `Int` because "give me a different arrangement" is a different verb from "set this number", and it
is the one actually wanted.

## Preview: a real edit, journalled and rolled back

Preview writes to the real scene and reverts it. Two alternatives were considered and rejected: a copy
of the component would have to be built and uploaded per dab and still would not answer the question
the preview is for (what does this do to *this* model, against its actual neighbours); and a shader
preview cannot show a geometry brush at all, because the geometry is the output.

What makes it safe is one rule on top of the journal: **preview writes never reach the undo history.**
So previewing cannot change the document — the worst case is a revert that has to be asked for — and
leaving the tab discards the lot.

**The palette is part of what gets taken back**, and missing that was the first version's real bug. A
brush resolves its roles into the palette *before* it paints a voxel, so a preview creates entries
whether or not it changes anything; reverting the voxels and not the entries left the document edited
by the one thing advertised as temporary — and edited in the structure whose numbering voxels store.
The entries are popped after the voxels are restored (by then nothing references them, so nothing is
renumbered), from the end, and only while the last entry is still one the preview made and still
unused. Both guards matter: the palette is not private to the preview.

Leaving is caught where `editor.mode` is written, not in the Lab's own button. The tab can be left by
the strip, by F10, by F11 straight to Render, or by the View menu, and a preview that survived any one
of those would have quietly become an edit with no undo entry. On a document swap the journal is
*dropped* rather than reverted: the geometry those coordinates described went with the scene, so there
is nothing to put back and a revert would write the old scene's cells into the new one.

## Known hazards

1. **Undo of an additive edit leaves empty Grid cells behind** — the standing issue in the SceneEditor
   README. A preview revert inherits it exactly.
2. **An entry the preview created and the user then adopted is not taken back.** The unwind stops at
   the first entry that is no longer the preview's -- renamed, built on top of, or still referenced by
   voxels the revert does not reach. That leaves an unused entry behind, which is untidy; the
   alternative is removing an entry somebody is using, which renumbers every slot above it.
3. **A binding to an unnamed entry is refused.** A binding is a name, and a photo-textured
   voxelisation gives nearly every entry no name at all. The Lab says so and points at the name field
   rather than silently naming the entry for the user — a palette edit nobody asked for, and one that
   would need its own undo entry.
4. **Scatter is declaration-only.** Preview refuses to run one and says why. A brush that silently did
   nothing would be indistinguishable from one whose parameters are wrong.
5. **The brush list is still an enum for the native brushes.** Sphere/Cube/Smooth/Bump/Extrude remain
   `SculptBrush` with its switch statements; scripted brushes are a separate path. Stage 2 is where
   those become one registry, and the five native brushes are what will prove the interface.

## Build order

1. **Lua, the brush model, the Lab, and preview** — done. Declaration, parameters (both kinds),
   diagnostics, hot reload, Material and Geometry evaluation, `skinDepth`/`crevice`/`normal`, the
   journalled preview and its revert, and material roles bound to palette entries with the shared
   Palette panel along the Lab's bottom edge.
2. **Scripted brushes as a tool** -- done, though not the way this said. They went behind a *seventh
   tool* (Custom, Ctrl+U) rather than behind Sculpt and Paint, and that turned out to be the simpler
   and the more honest arrangement: a tool is what a left-click means, and "run this Lua file" is a
   different verb from "add voxels with a sphere" rather than a variant of it. Folding them into Sculpt
   would have meant a brush picker that switched between two unrelated kinds of thing.

   The preview and the tool share every line except their two ends -- same dabs, same palette
   resolution, same fit test, same journal; one reverts and one records. That is what makes previewing
   a brush a truthful rehearsal of using it.

   What this stage did *not* do is fold the five native brushes into the same registry. They remain
   `SculptBrush` with its switch statements, which is still worth doing and is now the only part of the
   original stage 2 left.
3. **Scatter** — done. Sites from a lattice fixed to the model (not from the dab, which is what makes
   re-dabbing plant nothing new), an all-or-nothing fit test, `pv.solid`/`pv.fits` for what a brush
   needs to look at beyond its own voxels, and the rule that nothing is planted on what this stroke
   planted. All host services: every scatter brush wants them, and none should reimplement them.

   `brushes/trees.lua` is what proved it. Grass barely exercises a fit test -- three voxels fit
   anywhere -- while a tree is a thousand voxels of canopy and spends most of its sites being refused.
   It also turned up the one calibration fact worth writing down: **`crevice` reads about 0.6 on flat
   open ground**, because half the ball around a surface voxel is the ground it stands on. The first
   version of the tree brush skipped anything above 0.58 and so rejected every flat site in the scene,
   which presents as "scatter is broken" rather than as a threshold set one notch too low.
4. **Scatter output modes.** Blades *bake* into the target's voxels (half a million components is
   madness); trees *instance*, which is nearly free because `geometryPool` refcounts blobs — 200 trees
   sharing one blob cost one upload. Two hazards from the stamp system apply: `duplicateComponent` does
   not handle Grid components, and component deletion is a soft delete, so scatter must place instances
   directly rather than duplicate per tree.
5. **Composition.** A brush declaring a companion, so the grass tint and the grass tufts are one stroke
   and one history entry.
6. **A procedural brush source in compose.** So a brushed surface can be re-generated rather than only
   stored — the same gap `Part::procedural` has.

## Self-test

`BRUSHTEST=1 ./scene_editor <scene>` builds a 21³ slab in empty space inside an existing component,
runs every runnable brush on it, and tears it down. It covers what is invisible from the outside:

- **skin depth against known values** — a corner is 0, one voxel in is 1, four in is 4, the centre of a
  21-cube is 10, an empty cell is -1. A wrong depth field still produces a plausible texture, just one
  whose cracks are in the wrong places;
- **crevice ordering** — buried reads higher than an exposed corner;
- **the normal on the top face points up** — getting this wrong flips every "upward faces only" brush
  onto the undersides of things;
- **a revert restores every voxel's solidity and colour**, compared cell by cell over the slab and a
  margin shell around it. This is the promise the tab makes, and a revert that missed the cells a
  geometry brush *removed* would leave the document modified with no undo entry — the worst failure
  this system can have, and one nobody would notice until they saved;
- **a revert takes back the palette entries it created**, compared entry by entry (name and colour)
  before and after, and again after four overlapping dabs. This is the check the first version did not
  have, and the leak it would have caught shipped: the voxels came back and the entries stayed, so a
  preview quietly grew the palette of every model it was pointed at;
- **determinism** — run, revert, run again, byte-identical;
- **four overlapping dabs still revert exactly** — the property that makes a drag safe rather than
  just a click;
- **a bound role paints the bound entry's colour and costs the palette nothing**, measured as the
  difference between resolving the same brush bound and unbound rather than asserted flat — the brush's
  *other* roles still have to be created, so "adds no entries" is a claim about one role, not the brush;
- **the test leaves no geometry behind.**

And `brushtest.cpp` (not shipped; see the scratch harness in the commit discussion) covered the Lua
layer on its own: the sandbox has no reachable `os`/`io`/`require`/`load`, an endless loop is killed in
250 ms with a readable message, a syntax error and a missing `kind` are reported rather than crashing,
and per-voxel throughput is measured per brush.
