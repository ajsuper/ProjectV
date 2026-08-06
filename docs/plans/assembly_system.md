# Assembly System — Constructive Solid Modelling for the SceneEditor

**Implemented, and its container half superseded by [`asset_model.md`](asset_model.md).** This is
the design the fold was built from, and the fold, the ops, the bake and the compose round trip are
all still exactly this. What is gone is the *assembly* as a thing distinct from an asset: there is no
container object, no active-assembly pointer and no Assembly panel — the asset you have open is the
container, and its contents in order are the stack. Read this for how the CSG works and
`asset_model.md` for what owns it.

The built version is documented in
[`docs/examples/SceneEditor/README.md`](../examples/SceneEditor/README.md) under *Assets: the Place
and Region tools*, and lives in that example's `main.cpp`. Where the two differ, the README is what
the code does.

Four notes on what the build settled differently, or found:

- **`Bake ▸ To disk` writes the stack, not the resolved result.** Hazard 8 below asks for both, with
  the resolved `.data` marked as a cached result — which needs a second schema field to say "this
  entry is derived". Two honest verbs turned out to cover it without one: *As a new component*
  collapses the stack to one Chunk that every loader reads, and *To disk as an asset* writes the
  parts and their ops, which re-opens as the same editable assembly. Unbake is then free, because the
  parts on disk *are* the stack.
- **Adoption needed a case the plan does not mention.** `saveComposeToDisk` writes a node's
  *children* as the folder's component list, so an assembly saved as its own folder has no wrapping
  node and its ops land on root components. `adoptLoadedAssemblies` gives them one rather than
  refusing: the ops are the file saying these components combine, and an assembly is what that means.
- **Hazard 4 was real, and it had a sibling.** `deleteComponent` renaming only the root was fixed
  before Stage 1 landed, as the plan asks. `setComponentParent` had the same shape of bug and the
  plan does not mention it: it rebaked the moved subtree from the *ancestors'* accumulated transform
  and never multiplied in the moved component's own, so every reparent silently snapped the component
  to its new parent's origin. Nothing noticed because the hierarchy's drag-drop was the only caller
  and a component landing at its folder's origin reads as a quirk. The compose round-trip test caught
  it: every part came back correct and stacked at the origin, so the subtract cancelled the union
  exactly and the form resolved to nothing.
- **The gizmo needed no branch, but `applyComponentTransform` did.** The plan is right that one
  selection removes the `drivingStamp` branch. What it does not say is where the lattice snapping and
  cache invalidation go instead: into the transform funnel, so the gizmo, the arrow keys, the
  Inspector and the undo and redo of all three pick up the same behaviour without any of them knowing
  assemblies exist. Snapping has to be idempotent for that to be safe, which rounding to a lattice is.

A replacement flow for the Shape/Reference half of
[`stamp_system.md`](stamp_system.md). It keeps every mechanism that document built and changes what
those mechanisms are *arranged around*: the stamp stops being a thing in flight between a dropdown
and a target, and becomes a member of a persistent, re-editable boolean stack.

The workflow this is built for, stated as the user states it:

> Place primitives into the scene, move them around to get my shapes, merge them with boolean
> operations to create the forms I want, then sculpt for the final touch-up.

## Why the current flow fights that

Six things, each traceable to a line rather than to taste.

**1. The target is chosen before the shape exists, and can never change.**
`spawnShapeStamp(scene, editor, target, worldCentre)` builds the stamp at the *target's* voxel scale,
and the panel says so plainly: *"The target is fixed when a stamp is created."* So the first question
the Shape tool asks is "which existing component are you editing?" — and in this workflow the honest
answer is "none yet, I am building a new form out of primitives." `createShapeFromDropdown` even has a
fallback that goes hunting for *any* non-Asset component to point at, which is the tool admitting the
question was premature.

**2. There is no shape-to-shape boolean.** `MergeMode` is a property of a stamp describing what it
does **to the target**. Two floating stamps have no relationship to each other at all. "Box minus
sphere" is expressible only once the box is already committed scene geometry — so the very first
sentence the CSG workflow wants to say is the one sentence the model cannot represent.

**3. Merge is terminal.** `mergeSelectedStamps` writes the cells with `applyVoxelSculpt` and then
`releaseStampAt(..., true)` destroys the stamps. The composition that produced the form is gone the
instant it produces it; only the undo stack remembers, and only until it is trimmed. A form built from
five primitives cannot be revisited to move the fourth one two voxels left.

**4. Two selections, two panels, two lists.** `editor.selectedComponent` and
`editor.selectedStampIndex` are parallel selection systems. The Scene Hierarchy and the Reference
panel are parallel trees. `drawStampPlacement` and `drawReferencePanel` both render a stamp list and
both carry a Merge/Keep/Cancel row. The gizmo has to branch on `drivingStamp` to know which selection
it serves — which is exactly where the rotate-about-the-wrong-pivot bug lived, and that class of bug
is the tax on having two of everything.

**5. Committing forces one choice across two unrelated axes.** *Merge* means "resolve to voxels" AND
"write into somebody else's grid". *Keep as component* means "do not resolve" AND "become your own
object". The fourth combination — **resolve to voxels as a new object of my own** — is the one this
workflow is made of, and it is the one you cannot ask for.

**6. Lattice fidelity is asked about far too early.** `Snap 90`, *"free rotation — the merge rasterises
into the target lattice"*, *"too large to preview"*. All real, all correct, and all **bake-time**
concerns. Put in front of someone who is still sliding a cylinder around, they are noise attached to a
decision that has not come up yet.

## The central decision: an assembly is a real component

The same move `stamp_system.md` made, one level up.

> **An assembly is a live component whose children are the primitives, and whose voxels are the
> boolean resolve of those children.**

A stamp got to be a real component so it would inherit the raycaster, the gizmo, the outline, Sculpt
and Paint for free. An assembly gets to be a real component so it inherits the **hierarchy** for free:
parent/child is already in `ComponentRecord`, `localPosition`/`localRotation`/`localScale` already
compose down it, `getComponentWorldMatrix` already walks it, and moving a parent already moves its
children. A CSG stack is a parent with an ordered child list and one enum per child. Almost all of
that already exists and is load-bearing elsewhere.

The result — the resolved voxels you actually look at — is **today's preview component**, promoted
from debug aid to the thing the tool is for.

## What is reused verbatim

| Mechanism | Today | In the assembly |
| --- | --- | --- |
| `planStampMerge` | which target cells one stamp covers, and what was there | which assembly-lattice cells one part covers — unchanged |
| `refreshStampPreview` | builds a component holding a merge's output in a target lattice | builds the assembly's result, folded over the parts |
| the settle gate (`STAMP_PREVIEW_SETTLE_SECONDS`) | keeps the preview off the drag's critical path | the assembly's rebuild cadence — now load-bearing |
| `applyVoxelSculpt` | writes/removes cells | unchanged, at bake |
| `recipeFromStamp` / `materialiseStamp` / `StampRecipe` | restores stamps when a merge is undone | **the serialisable stack** — what makes an assembly re-openable and Unbake cheap |
| `resolveComponentVoxelSpace`, `snapStampToTargetLattice` | lattice maths | unchanged |
| gizmo, outlines, Inspector, edit history | | unchanged, and now with one selection instead of two |

The genuinely new code is small: an ordered fold over parts, one more enum value, and one panel.

## The flow

**1 — Place.** `Ctrl+T`, then **click in the viewport**. A primitive is seated where you clicked,
against the face under the cursor the way the sculpt brush seats its brush, or at
`shapePlaceDistance` if you clicked past everything. This restores what `PickPurpose::SpawnStamp`
was named for; today the tool's own hint has to say *"Create primitives from the dropdown in the Tool
panel"*, which is a tool explaining that its main verb is somewhere else.

If no assembly is active, the click creates one. The primitive is its first part.

**2 — Repeat.** Every subsequent click adds a part to the active assembly at the current kind and
size. Drop the pillar, drop the arch, drop the sphere you mean to carve with — no commit in between,
no target question, no mode.

**3 — Look at the result.** The viewport shows the **resolved** assembly: the parts folded together
in the assembly's lattice, rebuilt when everything settles. `Show parts` (today's *Preview merge
result*, inverted and renamed) switches to seeing the primitives themselves as ghosts. The default is
the result, because the result is what is being made.

**4 — Arrange.** The gizmo, the arrow keys, the Inspector's numeric fields. Unchanged — except that a
part is an ordinary component, so there is one selection system and the gizmo needs no branch.

**5 — Boolean.** The Assembly panel is the stack: one row per part in evaluation order, each with its
op, drag to reorder, click to select, `X` to delete.

```
Assembly  "Buttress"                     [Show parts]
 ┌──────────────────────────────────────────────┐
 │  ∪  Box       48x64x48                    ×  │
 │  ∪  Cylinder  24x64x24                    ×  │
 │  ∖  Sphere    32x32x32          selected  ×  │
 │  ∩  Box       40x40x64                    ×  │
 └──────────────────────────────────────────────┘
 4 parts · 18,204 voxels                Add ▾

 Bake  ▸ As a new component
        ▸ Into  Cathedral   [ ∪ ∖ ∩ ]
 Snap 90 ☑      within budget
```

**6 — Bake.** One verb, two destinations. *As a new component* is the missing fourth combination from
problem 5 and the default here. *Into `<selected>`* is today's Merge — and it carries its own op, so
carving a crater still works and now the carving tool can be a composed form rather than one
primitive. `Snap 90` and the budget readout live beside this button, where the question they answer
is finally being asked.

**7 — Sculpt.** The baked result is an ordinary component and every tool already applies. *Unbake*
restores the stack from the recipes if the bake was premature.

## Boolean semantics on a lattice

Three cell-set operations in the assembly's lattice, folded left over the parts from an empty
accumulator:

- **Union (∪)** — add the part's cells. The part's own colours win, which is what `Add` already does.
- **Subtract (∖)** — remove the part's cells. Writes no colour.
- **Intersect (∩)** — keep only cells in both. The accumulator's colours survive.

`planStampMerge` already produces the cell set each of these needs; the fold is the new part. `Add`
and `Subtract` map onto Union and Subtract exactly, so existing stamps carry over without a
migration.

## What this deletes

- `drawReferencePanel`'s duplicate Merge/Keep/Cancel row, and the second stamp list.
- The `drivingStamp` branch in `updateAndDrawTransformGizmo`, and `selectedStampIndex` as a selection
  system parallel to `selectedComponent`.
- "Which of the two panels do I use for this?"
- `MergeMode` as a per-stamp property meaning *versus the target* — it becomes a per-row op meaning
  *versus the parts above me*, which is the thing a user is actually thinking about.
- The `createShapeFromDropdown` fallback that hunts for any component to serve as a target.

## Staging

Each stage is shippable and leaves the editor in a coherent state.

**Stage 1 — the container.** An `Assembly` component kind; parts are its children; one selection; one
panel replacing Reference. Bake is still today's merge, and the ops are still Add/Subtract. No new
maths. This alone kills problems 4 and 5, and it is where click-to-place belongs too — it is cheap
and it is the largest single change in how the tool feels.

**Stage 2 — the resolve.** Fold the parts into a result component in the assembly's lattice; make it
the default view. This is `refreshStampPreview` generalised, and it inherits the settle gate directly.
Kills problems 2 and 6, and makes `Bake ▸ As a new component` real.

**Stage 3 — the stack.** Intersect, row reordering, per-row op controls, Unbake via the stored
recipes. Kills what remains of problem 3.

**Stage 4 — the polish.** Drag-to-size on placement, part duplication (`Ctrl+D` on a part is how you
get twelve identical columns), and an assembly that survives baking as a hidden, re-openable node.

## Hazards

**1. One assembly, one lattice.** Parts must share a voxel scale or the fold is a resample. So the
assembly owns `voxelScale` and a part inherits it at creation. This is today's "target fixed at
creation" constraint, relocated to somewhere it reads as a property of the object rather than as a
restriction on the tool — but it is the same constraint and it does not go away.

**2. Resolve cost scales with parts × OBB volume.** `STAMP_MAX_PREVIEW_CELLS` is a per-stamp budget;
the fold needs a whole-stack one, and it should be refused rather than truncated for the reason
`stamp_system.md` already gives about half-merged objects. Cache each part's cell set keyed on its
transform, so nudging one part re-walks one part and not ten. The settle gate is what makes the whole
thing affordable and must stay.

**3. Intersect on row 0 silently yields nothing** — an empty accumulator intersected with anything is
empty. Seed the accumulator with row 0 whatever its op says, and grey the op control on that row.

**4. `deleteComponent` half-deletes a subtree, and an assembly is the first thing that would notice.**
Its `disableSubtree` walk does kill every descendant's chunk and clear the children lists — but it
renames only the *root* to `__deleted__`. The descendants keep their names and their now-dangling
`parent`, and every flat scan in the editor filters on exactly that name: the palette combo
(`main.cpp:3097`, `:3117`) and `createShapeFromDropdown`'s target hunt (`main.cpp:7463`) would list a
deleted assembly's parts as live components pointing at dead chunks. Nothing deletes a parent with
children today, which is why this has never bitten; an assembly deletes one every time. Fix
`deleteComponent` to rename the whole subtree before Stage 1 lands — it is a two-line change and it is
the exact failure mode Hazard 1 of `stamp_system.md` was reverted for.

**5. Undo across a bake** has to restore the stack, not just the voxels. `recipeFromStamp` already
produces what the record needs to carry, and `mergeSelectedStamps` already demonstrates the pattern of
re-materialising stamps in an undo lambda.

**6. Sculpting a part before the bake** still works — a part is a component — but the sculpt is
against the *part's* lattice while what is on screen is the *result's*. Either hide the result while
Sculpt is active on a part, or accept a one-settle lag. The former is more honest.

---

# Assemblies and assets are the same object

An assembly is an asset that has not been baked yet. This is not an analogy — the engine already
models the asset exactly the way the section above proposes modelling the assembly, and the overlap is
close enough that most of "how do assemblies work" is answered by "the way assets already do".

## What already exists

**The assembly node is `ComponentKind::Asset`.** From `scene.h`:

```cpp
enum class ComponentKind { Chunk, Grid, Asset };
...
ComponentHandle parent;
std::vector<ComponentHandle> children;   // populated for Asset; empty for Chunk/Grid
core::vec3 localPosition;  core::quat localRotation;  float localScale;
```

A parent with children and a transform and no geometry of its own. That is the whole of what the
assembly node needs to be — and the SceneEditor **already creates them**: `Add Child ▸ Folder` and the
hierarchy's create menu both call `createComponent(..., ComponentKind::Asset, "New Folder", ...)`
(`main.cpp:2131`, `:2236`), with drag-drop reparenting already wired (`main.cpp:2050`). Stage 1's
container is a rename away from existing.

**The stack, on disk, is `compose.json`.** From `compose.h`:

```cpp
enum class ComponentType { Data, Asset };   // Asset: source is a folder with its own compose.json

struct ComposeComponent {
    ComponentType type;  std::string source;  std::string name;
    core::vec3 position;  core::quat rotation;  core::vec3 scale;
    Mutability mutability;
};
struct ComposeDoc { uint32_t version; std::string name; std::vector<ComposeComponent> components; };
```

An **ordered** list of named, transformed sources, where a source may itself be a folder containing
another such list. That is a CSG tree missing exactly one field.

**Instancing already works.** `GeometryBlob` is deduped and refcounted by `(sourceDataPath,
sourceBlockCoord)`, and chunks reference it through `geometryPoolIndex` rather than owning geometry.
Bake an assembly to a `.data` and place it fifty times, and the geometry exists once.

**Copy-on-write already works.** `Mutability { Locked, Direct, Copy }` — and `Copy` is precisely "the
edit is written to a new `.data`, the original untouched", which is what non-destructive baking wants.

**And the lattice constraint is the format's, not the editor's.** `DataFile` carries one `resolution`
and one `voxelScale` shared by every block in it. Hazard 1 above — *one assembly, one lattice* — is not
a restriction the editor imposes to keep the fold cheap. It is the invariant of the file the fold has
to produce. That is a good sign about the constraint rather than a bad one.

## The one missing field

```jsonc
{ "type": "data", "source": "sphere.data", "op": "subtract", ... }
```

`op` on `ComposeComponent`: `none` | `union` | `subtract` | `intersect`.

**It must default to `none`.** Every compose.json that exists today is a pure placement list; a
default of `union` would silently reinterpret every scene on disk as a boolean resolve. `none` also
makes the addition backward compatible in both directions — an older loader ignoring `op` reads a
composed asset as the placed parts, which is a degraded but coherent picture.

Because assets recurse, this one field gives **nested CSG for free**: subtracting a whole sub-assembly
is a child of `type: asset` with `op: subtract`. No new tree, no new evaluator — `loadComposeFromDisk`
already walks it.

## `op: none` is the rule that replaces Merge-versus-Keep

Problem 5 at the top of this document is that committing forces one choice across two axes. The `op`
field dissolves it, and does so **per part** rather than as a global verb:

| | `op: none` | `op: union / subtract / intersect` |
| --- | --- | --- |
| what it means | *placed* — parented, rendered as its own chunk | *resolved* — folded into the assembly's voxels |
| lattice | free; its own `voxelScale` is fine | must match the assembly's, or be resampled at bake |
| at bake | stays a separate `data` entry in the compose.json | disappears into the baked `.data` |
| today's equivalent | "Keep as component" | "Merge" |

So a cathedral asset can hold a baked stone buttress (`union`ed from primitives), a window asset
placed at its own finer voxel scale (`none`), and a carved-out doorway (`subtract`) — in one node,
without the editor having to ask a global question about what "commit" means. Composition by
placement never needed a shared lattice; only composition by boolean does. Stating that per part is
what lets both live in the same list.

## The pipeline closes into a loop

`stamp_system.md` established the shape: *"Build the stamp once and the two features become two
**sources** feeding it"* — shapes and lifted regions. **An asset on disk is source number three**, and
it is identical in every way that matters: a thing with geometry, a transform, and now an op.

```
        ┌─────────── sources ───────────┐
        │  primitive (procedural)       │
        │  lifted region (from scene)   │──▶  a part in an assembly  ──▶  bake  ──┐
        │  asset (from disk)  ◀──────────────────────────────────────────────────┘
        └───────────────────────────────┘
```

The machine's output is its own input. Bake a buttress, drop it into the cathedral's assembly as a
part, subtract a window from it. This is the thing that makes the flow feel like modelling rather than
like committing edits, and it costs one field plus a loader entry point.

**A primitive is an asset with a procedural source.** Rather than a separate concept, `ShapeKind::Box`
+ dimensions is just a `source` the loader can satisfy without touching the disk. Worth writing it
that way in the compose schema (`"source": "proc:box?w=48&h=64&d=48"` or a small object) so that a
saved assembly round-trips its primitives as primitives — still resizable after reload — rather than
as baked voxels. This is the difference between an assembly you can come back to and a mesh.

## The honest gap: the editor cannot write anything

`compose_io.h` offers `parseComposeJson` and `loadComposeFromDisk` — **and no writer**. `writeDataFile`
exists in the library but nothing in `main.cpp` calls it. The Library panel says so outright:

> *Nothing here loads geometry. … That is also why "bring this into the open scene" is not offered
> yet.*

So the asset story and the save story are one story, and two functions gate both:

1. **`writeComposeJson(path, ComposeDoc)`** — does not exist. Needed to persist an assembly as an
   asset, and needed for the editor to save a scene at all.
2. **`instantiateComposeInto(scene, folderPath, parent, transform)`** — `loadComposeFromDisk` builds a
   whole `Scene`; placing an asset needs the same walk grafted onto a subtree of a scene that is
   already open. This is `materialiseStamp` with the recipe read off disk.

`writeDataFile` and `parseComposeJson` cover the rest.

## What this does to the staging

The asset framing argues for pulling bake-to-disk **earlier** than Stage 4, because it is not a
convenience on top of the assembly system — it is the editor's first write path, and the assembly
system is the thing that finally motivates writing one.

- **Stage 1 — the container.** Unchanged, and cheaper than estimated: `ComponentKind::Asset` is
  already the node and the hierarchy already creates, reparents and renames it. Mostly a matter of
  teaching the Shape tool to parent into one, and retiring the Reference panel.
- **Stage 2 — the resolve.** Unchanged. Add `op` to `ComposeComponent` here, defaulted to `none`, so
  the field exists before anything depends on persisting it.
- **Stage 3 — persistence.** *(pulled forward)* `writeComposeJson` + `writeDataFile` behind
  `Bake ▸ As a new asset`. This is what makes an assembly reusable, instanced, and re-openable, and it
  gives the editor a save path it does not currently have.
- **Stage 4 — placement from the Library.** `instantiateComposeInto`, and the Library panel's
  deferred "bring this into the open scene" becomes drag-a-folder-into-an-assembly. Assets become
  source number three.
- **Stage 5 — the stack polish.** Intersect, reordering, Unbake, procedural sources that survive a
  round trip.

## Further hazards this raises

**7. `op` on a `Grid` component is meaningless.** A grid is many blocks across many cells; folding one
into a single-lattice `.data` is not a bake, it is a rebuild. Restrict boolean ops to `Chunk` and
`Asset` parts, and treat a `Grid` child as `op: none` regardless of what the file says.

**8. A baked asset loses its parts unless the compose.json keeps them.** If `Bake ▸ As a new asset`
writes only the resolved `.data`, the stack is gone and Unbake is a lie. Write both: the resolved
`.data` *and* a compose.json recording the parts that produced it, with the resolved entry marked as
the cached result. That is the same relationship a build system has with its sources, and
`Mutability::Copy` is the existing policy that describes it.

**9. Nested assemblies multiply resolve cost.** An asset with `op: subtract` must itself be resolved
before it can be subtracted. Cache the resolve per asset keyed on its parts' transforms — the same
cache Hazard 2 already asks for, applied one level up — and refuse rather than truncate past the
budget.
