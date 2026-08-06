# Asset Model — One Container, All The Way Down (SceneEditor)

**Built, Stages 1–4.** Stage 5 (collapsing the File menu's three save verbs) is not done; the
Assets panel's own **Save…** already acts on the open asset, so the menu items are now duplicates
rather than competing concepts.

It is a conceptual redesign that removes a concept rather than rearranging controls. It
supersedes the container half of [`assembly_system.md`](assembly_system.md) — the fold, the ops, the
bake and the round trip all survive unchanged; what goes is the idea that an *assembly* is a
different kind of thing from a *scene* or an *asset*.

The workflow this is built for, stated as the user states it:

> You have a list of assets. You can move the shapes around in these assets and use the existing
> paint/sculpt tools. You can switch assets by just clicking on the other asset in the list. You have
> per-asset save/load, and you can copy other assets into the asset you are working with. So instead
> of having a "scene" and then a bunch of assemblies, you just have lists of assets, and you can
> export a single asset, or you can copy it to another asset.

## The mistake this corrects

**The scene/asset split was never in the data model. The editor put it there.**

On disk there is no such thing as a scene. `compose.json` has exactly two entry types — `data` and
`asset` — and an `asset` is *a folder containing its own compose.json*. `loadComposeFromDisk` takes
any folder; `saveComposeToDisk` writes any subtree as one. What the editor calls "the scene" is
whichever folder happened to be opened at the top.

`assembly_system.md` then added a third container on top of that: an editor-only runtime object with
its own node handle, its own active-pointer, its own adoption pass at load — to express something the
format already expressed with one `op` field on a child.

So the window feels like it is doing too much because it holds **three overlapping container
concepts** for a format that has **one**. Every confusion downstream is a symptom:

- The Assembly panel sits on the left, empty, whenever you are not assembling — because it describes
  a container that may or may not exist.
- "Assemble" is a tool named after an object, sitting in a list of verbs.
- `resolveActiveAssembly` exists at all, because "which container am I adding to?" is a question with
  three possible kinds of answer.

## The central decision

> **There is only the asset. It is a folder with a `compose.json`. Everything else is a view of one.**

A scene is an asset. An assembly is an asset. A component of an asset is a thing you can open and
edit as an asset in its own right. There is no top, and no privileged kind.

The editor's job reduces to three verbs:

| Verb | Means |
|------|-------|
| **Open** | Make this node the edit root. It is what is rendered and what edits land in. |
| **Copy** | Bring another asset into the open one, where it becomes an ordinary component. |
| **Save** | Write the open asset back to its folder. |

## What is reused verbatim

The reason this is a redesign and not a rewrite: almost all of it is already built.

| Mechanism | Today | In the asset model |
| --- | --- | --- |
| `loadComposeFromDisk` | loads "the scene" | loads whichever asset you opened — same call |
| `saveComposeToDisk(scene, node, path)` | Save Scene / Save Folder as Asset | **Save**, for any asset |
| `instantiateComposeInto` | the Library's Import button | **Copy**, promoted to a first-class verb |
| `foldAssembly` + `writeDataFile` | the bake | **Copy ▸ Bake**, flattening on the way in |
| `ComponentRecord::op` + compose round trip | assemblies | unchanged — it is how shapes combine inside *any* asset |
| the breadcrumb | shows the selection's path | shows **where you are**, and clicking an ancestor steps out |
| `pullPartIntoLattice`'s Asset branch | folds an imported asset | unchanged |

The genuinely new code is a panel, a focus action, two copy modes, and the removal of ~1,900 lines of
assembly runtime.

## The flow

**1 — The asset list.** The left panel lists the assets in the folder you have open. Nothing is open
by default.

Each row has **two distinct verbs**, and that distinction is the whole interaction:

- The **disclosure triangle** expands the row to show what is inside it — the same folder-like tree
  the hierarchy shows today. This is a *view* change and nothing else.
- The **name** opens that asset: it becomes the edit root, it is what is rendered, and it is where
  new geometry lands.

Two verbs on one row, visibly different, is exactly what the current UI lacks — expanding a hierarchy
node and selecting it are presently the same gesture wearing two hats.

**2 — Open goes as deep as you like.** Expanding reaches past the folder's own `compose.json` into the
components of the assets inside it, and any of those can be opened alone. Opening the cathedral's
north tower to work on it in detail is the same operation as opening the cathedral: *make this node
the edit root*. There is one concept, not two, and the breadcrumb is the way back out.

This is what makes a large scene workable. You are never editing a million voxels because you are
looking at a tower.

**3 — Edit.** Every existing tool applies, unchanged, to whatever is open. Shapes are placed into it,
sculpted, painted, and combined with ops. This is the assembly system, minus the question of which
container it is acting on.

**4 — Copy.** Another asset is brought into the open one and becomes an ordinary component of it —
movable, editable, and part of what gets saved. See the three modes below.

**5 — Save.** Writes the open asset back to its folder.

## Copy, and why it replaces editing in context

The obvious missing feature looks like Blender's collection-edit or Unity's prefab mode: edit the
buttress while the castle is visible around it. This model does not have that, and does not need it.

**You copy first, then fit.** Once the buttress is in the castle it *is* part of the castle, so every
adjustment after that is already happening in final context. Prefab mode exists to let you edit a
thing while seeing where it goes; copying first means that problem never arises.

The cost is stated plainly: **a copy is unlinked, so there is no live instancing across assets.** Fix
a flaw in your tree asset and the two hundred already placed do not change. That is a deliberate
trade, and for voxel geometry it is very likely the right one — divergent copies per environment are
usually what you want, and the memory is shared anyway (see Copy below). But it should be a decision
rather than something discovered on a large map, which is why **Link** exists as a third mode for the
cases where it is not.

### The three modes

| Mode | On disk | In memory | Loses |
|------|---------|-----------|-------|
| **Copy** | Its own subfolder under the target, with its own `compose.json` and `.data` files | One pooled blob shared with the source until either is edited, then forked (`Mutability::Copy`) | Propagation from the source |
| **Bake** | One flattened `.data` entry | One blob, one chunk, one GPU header row | The internal structure — it stops being a stack |
| **Link** | A `type: asset` entry pointing at the source folder | Same as Copy | The ability to diverge |

**Copy** is the default and the one the workflow is built on. Note that "unlinked" is an *authoring*
property, not a storage cost: the geometry pool is keyed on `(path, block coords, mutability)`, so two
copies of a tree that nobody has edited are one blob.

**Bake** is `foldAssembly` run at import time, and it is the optimisation worth having: a
forty-component asset placed fifty times is two thousand header rows before baking and fifty after.
It is offered at copy time because that is when the choice is real — afterwards you would be
flattening something you may have already started editing.

**Link** is what the format has always supported and the editor never exposed: an `asset` entry that
resolves through to another folder at load. It needs one field the runtime does not have — see
Hazard 3.

## What this deletes

- `Assembly`, `AssemblyPart::assembly`, `editor.activeAssembly`, `assemblyIndexOf`,
  `resolveActiveAssembly`, `ensureActiveAssembly`, `adoptLoadedAssemblies`, `createAssembly`,
  `destroyAssembly`, `assemblyPalettePeriod` — most of the assembly runtime exists to answer "which
  container?", and the open asset answers it by construction.
- The Assembly panel, whose contents merge into the asset's own contents list. The stack *is* the
  contents, in order, with ops — they were never two lists.
- The Scene Hierarchy as an arbitrarily deep tree. Depth is navigated by **opening**, so the contents
  list is always one level deep and always readable.
- "Assemble" as a tool. Placing a primitive is a verb (**Place**); an assembly is not a mode you
  enter.
- The scene/asset distinction in the File menu: Save Scene, Save Scene As, and Save Selected Folder as
  Asset are three names for **Save**.

## The colour language

Colour means **kind**, consistently, in every panel that lists components. Today the hierarchy tints
`(folder)` amber and `(data)` blue and the Library tints scene folders pale blue, which is two
half-systems that disagree.

| Kind | Colour | Reads as |
|------|--------|----------|
| **Data** (Chunk) | blue | one voxel volume |
| **Grid** | teal | many blocks on one lattice |
| **Asset** (folder) | amber | a container you can open |
| **Linked asset** | violet | a container you do not own |
| **Derived** (a resolve result) | grey | scaffolding the editor rebuilds |

The point of separating **Asset** from **Linked asset** is that link-ness is otherwise invisible until
somebody edits one and is surprised that the other changed.

## Staging

Each stage leaves the editor working.

- **Stage 1 — the copy modes and the colour language.** ✅ **Built.** Additive: nothing is removed,
  and both are useful against the editor as it stands. `ComponentRecord::externalSource` is the one
  new runtime field, and `saveComposeToDisk` honours it. `ASSEMBLYTEST`'s thirteenth check asserts
  the three modes against the *files* they write, since that is the only place they differ.
- **Stage 2 — the asset list.** ✅ **Built.** `drawAssetsPanel` replaces `drawHierarchyPanel` *and*
  `drawAssemblyPanel`; the left column is two panels rather than three. Expand and Open are two hit
  targets on one row — the arrow and the trailing `open` button — with Select on the name.
- **Stage 3 — focus.** ✅ **Built.** `editor.openAsset` is the edit root, breadcrumbs in both the
  panel and the viewport step back out, and the contents list follows it. **Isolate** draws only what
  is inside it.
- **Stage 4 — unwind the containers.** ✅ **Built.** `syncResolves` derives the fold from the ops once
  a frame; `editor.activeAssembly`, `resolveActiveAssembly`, `adoptLoadedAssemblies`, `adoptAssembly`
  and `AssemblyPart::assembly` are gone. `Assembly` is now `Resolve` and is derived state with no
  user-facing existence. "Assemble" is **Place**.
- **Stage 5 — one Save.** *Not built.* The Assets panel's Save… acts on the open asset already, so
  File ▸ Save Scene / Save Scene As / Save Selected Folder as Asset are duplicates of it rather than
  a competing model. Collapsing them is a menu edit, not a structural change.

## What the build settled differently

**Click-to-open became double-click-to-open, plus a button.** The plan had the *name* open an asset.
Every tool aims at `editor.selectedComponent`, so a single click on a name has to select or the
Sculpt brush cannot be pointed at anything one level down. The row therefore carries three targets,
visibly different: the arrow expands, the name selects, and `open` (or a double-click on the name)
opens. Two verbs on one row was the plan's whole point; this is three, and the third is the one that
already existed.

**Isolate is a checkbox, not what Open does.** Opening is navigation. Hiding the rest of the document
without being asked is a surprise, and it fights the resolve, which owns the sources-versus-result
question inside an asset. So isolation is applied on the *edge* (`EditorState::isolationDirty`),
walks the tree once, and `refreshResolves` skips assets it has hidden rather than folding work nobody
can see.

**`wrapRootStack` survived.** Everything else about a resolve is derived per frame, but this one
edits the graph rather than reading it — `saveComposeToDisk` writes a node's children as the folder's
component list, so an asset saved as its own folder returns as root components carrying ops. It runs
at load, once. Hazard 1 does not bite, because `openAsset` refuses anything that is not an Asset
node: the open asset always has a folder of its own to be saved to.

**The op glyphs became ASCII.** `∪ ∖ ∩ ·` are outside the default ImGui font's range, so every op
control in the editor rendered the same missing-glyph box — four operations that looked alike, in
the one place where telling them apart is the job. They are `+ - & .` now.

**The Place panel needed the same split the containers did.** `editor.shape*` was doing two jobs —
the template for the next shape *and* the recipe of the selected one — so the fields kept the
previous selection's numbers and the first nudge after clicking a second shape resized it to the
first one's dimensions. `regeneratePart` reads only the `Part` now; the panel edits the `Part` when
one is selected and the template when none is; and the kind is read-only on a placed shape, because
substituting a sphere for a box is not a resize. `ASSEMBLYTEST` 16 asserts it.

**Two self-tests were added rather than re-pointed** (Hazard 6). The twelve fold checks all still
run through `foldNode` and needed nothing. What they did not cover is the machinery that replaced the
container: `ASSEMBLYTEST` 14 asserts that a second placement lands in the *same* asset, and 15
asserts `syncResolves` in both directions. Both were confirmed to fail when the mechanism they guard
is disabled. 16 came later, from the panel split above.

## Hazards

**1. Focus and save do not always mean the same node.** A `data` child lives inside its parent's
`compose.json` and has no folder of its own, so saving while focused on one must save the nearest
ancestor that *is* a folder. An `asset` child has its own folder and can save alone. Same key, two
behaviours — the UI has to say which one it is about to do, or a save will silently write more than
was expected.

**2. Copy must not write through to the source.** For a copy to be genuinely unlinked, the target's
folder needs its own subfolder for it. Writing a `type: asset` entry pointing back at the source is
exactly what **Link** means, so getting this wrong does not fail — it silently produces the other
mode, and nobody finds out until they edit one and break the other.

**3. Link needs a runtime field, and round-tripping it is the whole point.** `ComponentRecord` has
`sourcePath` (provenance, currently unused for `asset` nodes) but nothing that says "this subtree is a
reference; write it as one and do not serialise its contents". Without that, a link survives until the
first save and then quietly becomes a copy. The relative path also has to be recomputed against the
output folder, since saving to a new location invalidates a stored relative path.

**4. One asset open at a time is forced, not chosen.** Holding two scenes' textures resident is what
killed a Vulkan driver and why loading is two-phase. `instantiateComposeInto` already sidesteps this
by loading into a temporary `Scene` and dropping it after the graft, which is exactly the shape Copy
needs — but it means "open two assets side by side" is not available to add later without solving
that first.

**5. Bake needs a lattice before it has an owner.** The imported asset arrives at its own voxel scale;
the fold has to happen in *some* lattice. When the target is an asset with a voxel scale, that is the
answer. When it is empty, the imported asset's own scale is the only honest choice, and it then
becomes the target's.

**6. Deleting the assembly runtime deletes the tests with it.** `ASSEMBLYTEST`'s twelve checks cover
the fold, the ops, the ordering rule, the pull-not-push property and the compose round trip — all of
which survive this redesign and none of which should be lost with the container that currently owns
them. They need re-pointing at the open asset before Stage 4, not after.
