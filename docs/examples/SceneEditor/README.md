# SceneEditor

ProjectV's scene editor / modelling tool: a window whose interface is made of **ImGui dock panels**, one of which is the scene rendered live by the engine, with **File ▸ Load Scene…** loading any Compose scene folder off disk at runtime.

Editing is organised around **tools** — Select, Move, Sculpt, Paint — chosen from a strip of icons down the left edge of the scene, or with `Ctrl+Q`/`W`/`E`/`R`. The tool is what a left-click in the viewport means, and the right-hand column shows the tool's settings. Select and Move are complete; Paint recolours voxels along a click-and-drag, and Sculpt adds and removes geometry along one. Palette editing — recolour, rename, add and remove entries — is complete, and everything is undoable.

## The shell it is built on

**One frame carries both the scene and the interface.** The scene's render passes draw into an offscreen texture instead of the back buffer, and ImGui draws that texture as an image inside the Viewport panel. That is what lets the scene live in a dock node the user can move, resize, and tab like any other panel — rather than being the window, with the interface floating on top of it. Every editor tool that will want a panel beside the scene depends on this.

**The scene is a replaceable runtime resource.** The previewer loads one scene at startup and lives with it. An editor cannot: File ▸ Load Scene… tears down the GPU state (`destroyGPUData`) and uploads a different scene (`createTexturesForScene`) without a restart, and a bad path leaves the scene that is already open untouched.

The viewport renderer itself is the [ScenePreviewer](../ScenePreviewer/)'s — one primary ray per pixel, pure albedo, no lighting — with the display pass retargeted from the back buffer to an offscreen texture. It is the right renderer to start an editor from: it shows what is actually *in* the scene rather than how it is lit, and at one ray per pixel it leaves the frame budget for interface and, later, editing.

## How to Build

```bash
cd SceneEditor
make               # builds ./scene_editor
./compEditor.sh    # (re)compiles the viewport and ImGui shaders to .bin
```

Requires ProjectV built at `../../../` with its libraries in `../../../lib/`, bgfx built, and the ImGui submodule checked out:

```bash
git submodule update --init external/imgui
```

## How to Use

```bash
./scene_editor [scene-directory]
```

With no argument it opens the previewer's bundled `StonehillCastle` if it is there, so the editor starts on something rather than an empty panel. Either way **File ▸ Load Scene…** switches scenes.

| Input | Action |
|-------|--------|
| Right-mouse drag in Viewport | Fly the camera (cursor is captured for the duration) |
| `W`/`S`, `A`/`D`, `R`/`F` | Forward/back, strafe, up/down — while flying |
| Scroll wheel | Movement speed — while flying, or hovering the Viewport |
| `H` | Re-frame the camera on the scene |
| Left-click in Viewport | Whatever the active tool says — see Tools below |
| `Ctrl+Q` / `Ctrl+W` / `Ctrl+E` / `Ctrl+R` | Select / Move / Sculpt / Paint |
| `Ctrl+T` / `Ctrl+Y` | Shape / Region |
| `Ctrl+O` / `Ctrl+Shift+R` | Load Scene… / reload the current scene |
| `Ctrl+Z` / `Ctrl+Shift+Z` | Undo / redo |

While a stamp is floating (see **Shape** and **Region** below), the keyboard also means:

| Input | Action |
|-------|--------|
| Arrow keys | Nudge one voxel of the target's lattice, in the camera's ground plane |
| `PgUp` / `PgDn` | Nudge one voxel vertically |
| `Ctrl` + arrows | Turn 90° about the lattice axis most nearly facing the camera |
| `Enter` / `Esc` | Merge into the target / cancel |

Panels dock, tear off, and tab by dragging their title bars. The layout is saved to `imgui.ini` beside the executable and restored on the next run; **View ▸ Reset Layout** puts it back to the default.

### The layout

Two columns and a strip, which is the shape editors of this kind converge on for a reason — the left column answers *what exists*, the right one *what is being done to it*:

```
┌─────────────┬──────────────────────────────────┬─────────────────┐
│ HIERARCHY   │  StonehillCastle › model         │ INSPECTOR       │
│   what is   │  ┌──┐                            │ TOOL            │
│   in the    │  │QW│        viewport            │ PALETTE         │
│   scene     │  │ER│                            │                 │
├─────────────┤  └──┘                            │                 │
│ LIBRARY     │         [AO] [normals]           │                 │
│   what      ├──────────────────────────────────┴─────────────────┤
│   could be  │ HISTORY                                            │
└─────────────┴────────────────────────────────────────────────────┘
  1.4 ms (700 fps) | 2663 x 1303 | 6 chunks, 2 components | Select | …
```

The right-hand three are **stacked, not tabbed**, and that is the point: they are not alternatives. Sculpting wants a brush, a colour, and the target's transform in view at once, and while they shared one dock node the tool panel had to carry its own duplicate copy of the palette grid — two places showing "the current colour", which is how you end up sculpting in a colour you were not looking at.

### Panels

| Panel | Shows |
|-------|-------|
| **Viewport** | The scene, rendered at exactly the panel's pixel size, with the breadcrumb, the tool strip and the render toggles floating over it |
| **Scene Hierarchy** | The component tree from `compose.json` — Asset folders with Chunk/Grid leaves |
| **Library** | A disk browser over the folders scenes live in: navigate, see which are Compose scenes, list what is inside one without loading it, open it |
| **Inspector** | The selected component: kind, source, voxel count, local and world transform |
| **Tool** | The active tool's settings. Title and contents follow the tool — a setting the current brush or shape cannot use is **absent**, not greyed out, since a disabled control still reads as part of the tool and still has to be ruled out |
| **Palette** | The selected component's materials as a grid of swatches — edit colours, add and remove entries, see where each is used |
| **History** | Every edit made, with undo/redo and click-to-jump |
| **Statistics** | Frame time, camera, accumulated samples — a floating window under **View ▸ Statistics**, not a dock panel |

The status bar along the bottom of the window carries the summary permanently: frame time, viewport resolution, scene size, active tool, and the last message. Messages also float over the viewport for a few seconds after they change, because most of them answer something the user just did *there* — reading the answer at the far corner of the screen was the whole problem with keeping them in a docked panel.

## Tools

The tool is what a left-click inside the viewport means. Without one, every new interaction has to negotiate for the same button: the gizmo wants a drag, picking wants a click, and a sculpt stroke wants a drag over the same pixels the gizmo is drawn on. A mode makes that a choice stated once rather than a modifier remembered per action.

`Ctrl` is part of every shortcut because the bare letters are already spoken for — `W`/`A`/`S`/`D`/`R`/`F` fly the camera while the right button is held, so unqualified `Q`/`W`/`E`/`R` would mean a fly-through that silently changed tool on the way past.

| Tool | | Left-click does |
|------|---|-----------------|
| **Select** | `Ctrl+Q` | Selects the component owning the voxel under the cursor. Clicking past everything clears the selection |
| **Move** | `Ctrl+W` | The transform gizmo: three arrows to translate, three rings to rotate. Clicking away from a handle still selects |
| **Sculpt** | `Ctrl+E` | Drag to add or remove voxels with a Sphere or Cube brush, to Smooth or Bump a surface, or to Extrude a whole face. `Alt`+click samples instead |
| **Paint** | `Ctrl+R` | Drag to repaint voxels with the palette's current entry, in one of five shapes (the two fills run once per click). `Alt`+click samples instead |
| **Shape** | `Ctrl+T` | Drops one of seven primitives into the scene as a floating stamp, on the surface under the cursor or `Place distance` down the ray when there is nothing there. `Alt`+click samples instead |
| **Region** | `Ctrl+Y` | Selects voxels already in the scene — two corners of a box, a face, or a connected volume — to copy, cut, delete or recolour. `Alt`+click samples instead |

`Ctrl+Y` used to be a second binding for Redo. The keyboard-row logic wants `Q`/`W`/`E`/`R`/`T`/`Y` in order, and Redo keeps `Ctrl+Shift+Z`, which the Edit menu has always advertised alongside the alias.

A miss deselects only under Select, whose whole job is choosing: clicking past everything is how you say "nothing". Under the other tools a near miss is a slip of the hand, and losing the selection — and with it the gizmo, or the brush's target — would cost far more than it saves.

Selecting from the viewport also **opens and scrolls the hierarchy** to the component that was picked. Selecting something the panel meant to show it cannot show is not a selection.

### The breadcrumb

A strip along the top of the viewport reading `scene › folder › … › selection`, every element clickable. The thing an edit lands on is the selection, and until now the only place that said so was a highlighted row in a panel that might be scrolled away from it. A tool about to add or remove voxels needs its target stated where the voxels are — and the ancestors being clickable makes "work on the whole asset instead" one click rather than a hunt back up the tree.

### Sculpting

Hold the left button and drag. Every frame casts one ray, turns where it landed into a cell of the target component, and stamps the brush there — so the geometry appears under the cursor as it is drawn, not at the end.

| Brush | One dab covers |
|-------|----------------|
| **Sphere** | A ball of voxels centred on the cell under the cursor (radius in voxels, up to 24) |
| **Cube** | A box centred on it, aligned to the component's axes (up to 48 a side) |

**Add** puts voxels in the empty cell just outside the face the ray hit; **Remove** takes the cell that is there. Additions use the palette's current entry, exactly as Paint does, and a component **grows to fit them** — a loose Chunk becomes a Grid and a Grid expands (`applyComponentQueue`), so a stroke is not confined to the bounds the component happened to load with.

With nothing under the cursor the dab lands `Place distance` world units down the ray. That is the only way to put the first voxel into a component that is still empty, and it is also what keeps a stroke flowing when the drag runs off the edge of the object it started on.

Two more entries — **Smooth** and **Bump** — [reshape rather than place](#smooth-and-bump), and the last, **Extrude**, is not a brush at all — see [below](#extrude).

#### Why the ray ignores the stroke's own geometry

This is the whole difficulty of the tool, and it is not a subtlety — it is the difference between a brush and a catastrophe.

A stroke edits the scene it is casting into. By the second frame the scene already contains the first frame's dab, which sits *nearer the camera* than the surface did. Believe the scene and the next dab goes on top of that one, and the one after on top of that: an additive drag walks a column of voxels straight back up the ray toward the camera, a brush-width per frame, for as long as the button is held. Removal has the mirror version — the ray drops through the hole it just opened and eats a tunnel through the object.

So for as long as the button is held, the ray is shown **the surface the stroke started against**. `utils::pickVoxel` takes an optional `VoxelSolidityOverride`, called for every cell its traversal steps into — not only the solid ones, because forcing an *empty* cell back to solid is half of what it is for:

* a cell this stroke **added** reports empty, so the ray passes through and keeps finding the original surface;
* a cell this stroke **removed** reports solid, so the ray still stops there and the brush does not sink.

`sculptStrokeTouched` is that set: every cell the stroke has changed, and only for the stroke's own component — everything else in the scene answers normally, so a drag that wanders across a second object still lands somewhere sensible.

A voxel forced solid has no material to report, because it is not in the tree any more. Such a pick carries slot 0, so **never hand a solidity override to anything that reads `materialSlot`** — a sampler or the eyedropper.

#### The rest of a stroke

A stroke locks its **component**, its **mode** and its **colour** the moment the button goes down. An edit queue belongs to a component, so a drag that wandered onto a second object would otherwise start writing into it halfway through — one gesture, two undo entries, and geometry where nobody was pointing.

Dabs are **interpolated between frames** at half the brush radius. A drag is sampled once per frame, so how far the cursor travels between dabs depends on the frame rate and how fast the mouse is moving; without interpolation a quick flick lays a dotted line of disconnected blobs. Past 48 steps in one frame the stroke thins out rather than the frame stalling — a dropped frame mid-stroke is far more disruptive than a slightly sparse one.

Everything one frame produced goes out in **one** `queueVoxelAdd`/`queueVoxelRemove` + `updateScene`, for the same reason painting queues a whole stroke at once: `updateScene` forks and rebuilds every chunk a queue touches. And the whole drag is **one** history entry — a drag is a single gesture, and undoing it should put the scene back to where the button went down.

#### Smooth and Bump

The two brushes that reshape rather than place. Neither stamps anything: each runs one pass of an operator over the cells inside the brush sphere and **repeats it on a tick** — ten times a second — for as long as the button is held. That is what makes them behave the way a physical tool does, hold longer and get more, and it is why they work with the cursor standing still, unlike the shape brushes. A tick is a fixed unit of effect rather than a frame's worth, so holding for "about a second" means the same thing on any machine.

**Smooth is a majority filter** over the 3×3×3 neighbourhood including the cell itself: solid if strictly more than 13 of the 27 are (the [Strength](#smooths-strength) setting can widen that neighbourhood, and below full strength it narrows what the filter is allowed to act on). A majority filter *is* a smoothing operator on a voxel grid — it is stable on a flat surface (a surface cell sees 18 of 27 and stays; the cell above it sees 9 and stays empty), shaves anything that protrudes, and fills anything dented in. Rounding convex features and filling concave ones falls out of the same test rather than needing two.

**Bump** dilates or erodes by one layer per tick through face neighbours only, in the colour of whatever it is growing out of, followed by however many smoothing passes the `Blend` setting asks for. Dilating everything inside a sphere on its own gives a disc with a cliff around it — the bump reads as "a sphere was stamped here" rather than as the surface being pushed — and the blend has to reach *past* the brush to fix it, because the join it is rounding off is exactly at the rim.

##### Smooth's strength

One slider, `0.00`–`3.00`, and **1.00 is the line between two different things**. Below it the filter stays 3×3×3 and the strength decides which cells it may touch; above it the filter widens. The default is `1.00`, which is the plain majority filter the brush has always been, so the setting changes nothing until it is moved.

**Below 1 — how out of place a cell has to be.** Everywhere else "strength" is how far each point slides toward the smoothed result, which needs a continuous surface to slide along. Here a cell is solid or it is not, so the only thing a weakened filter can choose is *which* cells it flips, and the honest ordering is by how badly each one disagrees with its own neighbourhood. Distance from the threshold is exactly that number, and it is symmetric — a solid cell flips at 13 neighbours or fewer, an empty one at 14 or more, so both sides run from 1 (a hair over the line) to 13 (an isolated voxel, or a hole with solid on every side). The slider is a floor on it:

| Strength | Cutoff | Spares |
|----------|--------|--------|
| `1.00` | 1 | Nothing. Rounds edges and corners as well as noise — the aggressive end |
| `0.75` | 2 | A clean 90° edge (about 2), which is the first thing users notice being eaten |
| `0.25`–`0.50` | 4 | Staircase steps and gentle curvature; still fills a one-voxel pit and shaves a one-voxel spike (both about 4) |
| `0.00` | 6 | Everything but noise: a lone voxel and a fully enclosed hole are 13, and very little else reaches 6 |

**Above 1 — how large a feature the filter can see.** Past `1.00` the cutoff has nothing left to give: at 1 the filter already flips every cell it can. So the strength widens the neighbourhood instead, one step per whole number — `2.00` is a 5×5×5 majority (63 of 125), `3.00` a 7×7×7 one (172 of 343).

That is not the same as holding the button longer, and the difference is the whole point of the range. Holding reruns the 3×3×3 filter on its own output, and it has **fixed points**: a two-layer slab, a large sphere, a dome spread over a dozen voxels are all things it will never touch, because at the one-voxel scale they are already flat. A wider kernel asks the same question over a larger piece of the surface and sees them. The self-test proves exactly this — a two-layer slab survives four ticks at strength 1 untouched, and one tick at strength 2 takes the whole thing (50 solid of 125 where it wants 63).

> ⚠️ **A body thinner than the kernel dissolves rather than smooths.** That is what "smooth at a five-voxel scale" means for something two voxels thick — and voxelised meshes are usually hollow shells a few voxels thick, so a wide setting aimed at one takes it out. The Tool panel says so beside the slider.

Both halves have to be **selective rather than merely slower**, since these brushes repeat on a tick and a setting that only delayed the same verdict would converge on the same result within a second and be no setting at all. Below 1 that means a weak pass held for four ticks takes the floating voxel and still leaves the spike and the dent; above 1 it means reaching a shape that no amount of holding at 1 ever reaches.

Widening costs the kernel's volume per cell — 343 array reads against 27 — so the **colour vote deliberately stays on the inner 3×3×3** and runs only for cells actually being filled. Widening the kernel widens what counts as rough, not what a cell is made of, and a vote over 343 cells would be both wrong (colours from three voxels away are not what this cell adjoins) and, on a photo-textured scene where nearly every voxel has its own palette entry, the most expensive thing in the tool. With that, a radius-10 tick measures **0.35 ms at strength 1 and 1.00 ms at strength 3**, against a 100 ms budget.

Bump's own blend passes ignore the setting entirely and always run at 3×3×3, cutoff 1. They exist to kill the cliff at the brush's rim; a weakened pass would spare exactly that, and a widened one would reach well past it.

#### Why these two are affordable

**The cost is reading the geometry, not writing it.** Both operators ask about a cell and then about the cells around it, and neighbouring cells overlap heavily — a 3×3×3 count re-reads 18 of the 27 its neighbour just read. Asked of the tree64 one at a time that is ~28 descents per cell: half a million per tick of a radius-10 Smooth, five million a second at ten ticks. Copying the box into a flat array first makes it one descent per cell and turns every pass into array indexing, and the same tick drops to ~17k descents and **0.35 ms** against a 100 ms budget. Widening the kernel multiplies the array reads, not the descents, which is what keeps [strength 3](#smooths-strength) affordable at 1.00 ms.

Every cell of a pass is decided against the state at the *start* of that pass, gathered first and written afterwards. Deciding against a half-updated array would make the result depend on the loop's direction — a pass sweeping in +x would smooth differently from one sweeping in −x, and a bump would run away across the sphere in a single tick instead of growing one layer.

Both read the **current** geometry rather than the stroke's starting state, which is a deliberate exception to [the lie told to the ray](#why-the-ray-ignores-the-strokes-own-geometry): each pass must build on the last, or holding the button would do the same thing forever. It is safe because neither takes its *position* from the geometry — the ray still finds the original surface, which is what keeps the brush parked where it was aimed, and only the operator inside the sphere looks at what is actually there now.

#### The world lattice

Sculpting needs something painting never did: a coordinate for a cell that **does not exist yet**, because additive strokes create cells. `ComponentVoxelSpace` therefore carries the component's lattice in world space — a Grid lays its cells at `origin + R * (cellIJK * cellSize)` and each cell subdivides into `resolution` voxels per axis, so cells and voxels fall on one lattice and there is a single mapping for the whole component rather than one per cell:

```
centre(coord) = latticeOrigin + R * ((vec3(coord - coordOrigin) + 0.5) * voxelSize)
```

`componentVoxelToWorld` and `worldToComponentVoxel` are that mapping and its exact inverse. `coordOrigin` is `originCellCoord * resolution`, and is non-zero only after a Grid has expanded *downward* — which moves `grid.origin` without moving any existing geometry, and is exactly the case that gets this wrong silently.

### Extrude

Sits in the brush list but works nothing like the other two. Click a face, drag, and that whole face moves. Nothing is stamped along the path of the cursor; the shape of the result is the shape of the face, and the cursor supplies exactly one number — how far.

**What counts as "the face you clicked on"** is every solid voxel that

* lies in the same plane as the one under the cursor,
* has its face open in the same direction — part of the *surface*, not buried behind it,
* is reachable from the clicked voxel through face neighbours within that plane, and
* under the `Material` [scope](#selection-scope), uses the same palette entry.

Connectivity is 4-way within the plane, for the same reason the fills are 6-way: diagonal connectivity leaks across the gap where two faces touch only at a corner. `gatherFaceRegion` is shared with Paint's Fill face, which selects exactly this region and recolours it instead of moving it.

The voxels pulled out take **their own source voxel's material**, not the palette's current entry. Extruding a wall extends that wall; being handed whatever colour happened to be selected in another panel is not what "pull this face out" means. Which way the face goes is which way you drag — out to extend, back in to carve, and past the original face it keeps cutting — so the tool has no Mode setting, no size, and no Place distance, and the panel does not show them.

#### Why it needs no solidity override

The brush casts a ray every frame and therefore needs the [override](#why-the-ray-ignores-the-strokes-own-geometry) to keep from chasing its own deposits. Extrude fixes its face and its axis on the press and reads depth by projecting the cursor onto that axis with `closestPointOnAxis` — the same projection a translate gizmo handle uses. The geometry it creates was never in a position to mislead it, because after the press it never asks the geometry anything.

#### The layer records

`extrudeAppliedDepth` is signed and is the whole committed state: positive layers added outward, negative carved inward, zero untouched. Every frame walks it toward whatever the cursor now says, **one layer at a time**, which is what makes a drag reversible in place — pulling out to five and back to two leaves exactly the state a drag straight to two would have produced.

Each applied layer records what it *displaced*, split two ways: cells that were empty (reverse by removing) and cells that held something (reverse by writing their old contents back). The naive version — "pulling a face out fills empty space, so retracting empties it again" — is wrong the moment a face is extruded into geometry that was already there: a stair tread pushed into the tread above it, a wall pushed into a floor. Retracting would delete voxels the drag never created, and so would undo. With the records, reversing a layer is one rule in both directions.

### Painting

Hold the left button and drag. One dab covers one of five shapes, chosen in the Tool panel:

| Mode | Covers |
|------|--------|
| **Voxel** | Just the one under the cursor |
| **Sphere** | Every solid voxel within a radius of it (radius in voxels, up to 32) |
| **Cube** | Every solid voxel in a box centred on it (width/height/depth in voxels, up to 64) |
| **Fill face** | Across the one surface you clicked: same plane, face exposed, 4-connected |
| **Fill volume** | Through the solid: 6-connected in three dimensions, inside included |

All of them **only recolour voxels that already exist** — a paint brush never creates one. Adding geometry is the Sculpt tool's job, and a brush that quietly filled the empty space it passed over would be the most surprising tool in the editor. They also all stay inside the component that was clicked, because an edit queue belongs to a component.

#### The stroke

The first three shapes are **swept**, exactly like the [sculpt brush](#sculpting): every frame the cursor moves casts one ray, turns where it landed into a cell of the target component, and stamps a dab there. Repainting a wall is a sweep over it, not one click per brush-width.

A stroke locks its **component** and its **colour** the moment the button goes down, for the same reasons the sculpt stroke does: an edit queue belongs to a component, and one gesture should be one entry in the history. Dabs are **interpolated between frames** at half the shape's smallest extent (one voxel for the `Voxel` shape), because a drag is sampled once per frame and without interpolation a quick flick lays down a dotted line. Past 32 steps in one frame the stroke thins out rather than the frame stalling, and the status bar says so.

Everything one frame produced goes out in **one** `queueVoxelAdd` + `updateScene`, and the whole drag is **one** history entry.

**The two fills take no part in the drag.** They run on the press and the rest of the gesture is ignored — a bucket fill is a click everywhere else, and re-running one every frame would re-walk a region of up to four million voxels to discover the previous frame had already painted all of it. The Tool panel says which of the two the current shape is.

Where the sculpt stroke needs [an elaborate lie told to its own ray](#why-the-ray-ignores-the-strokes-own-geometry), this one needs nothing: painting never changes what is solid, so the ray sees the same surface on the last frame of the stroke as on the first. It needs no journal of original state either — `collectPaintTargets` refuses to collect a voxel that is already the colour being painted, so a voxel this stroke has covered is skipped on every later frame and reaches the undo record exactly once. The only repeats it can produce are *within* one frame, where consecutive dabs of an interpolated run overlap and the scene has not been written yet; those are dropped before the queue.

**The two fills are kept apart deliberately**, and the difference between them is the difference between recolouring a wall and recolouring the building. A volume fill spreads in three dimensions through anything it touches, so it reaches the *inside* of a shape as readily as the outside and does not stop at a corner the way someone looking at the surface expects. A face fill stays on the surface pointed at — it is the same region the [Extrude](#extrude) tool moves, recoloured instead of pulled. Both take the [scope toggle](#selection-scope) below; `Fill volume` + `Whole volume` is the one combination that repaints an entire connected model in a click, which is occasionally what is wanted and never what is wanted by accident, so it takes two deliberate choices to reach.

Sizes are in **voxels**, not world units: this is a tool that addresses the voxel grid, and a radius in world units would mean a different number of voxels per component depending on each one's `voxelScale`. The world size is shown underneath for the number you actually picture.

Under the `Material` scope both fills compare **palette entries, not colours**, so two entries that happen to hold the same colour stay separate regions — they are separate materials, and merging them would recolour more than was pointed at. Both spread through face neighbours only; diagonal connectivity leaks a fill through the gap where two walls touch at an edge.

### Stamps: the Shape and Region tools

Shape placement and region copy/move look like two features. They are one machine, and building it once is what makes the second one nearly free.

Both produce a **floating stamp**: geometry that is not part of the scene yet, carrying its own transform, driven by the transform gizmo, ending in a commit or a cancel. The two tools are two *sources* feeding it — Shape generates a primitive, Region lifts voxels that are already there — and from the moment it is floating, nothing downstream can tell which made it.

The decision the rest follows from is that **a stamp is a real `ComponentKind::Chunk` component in the scene**, not a CPU-side voxel buffer with a preview overlay. A buffer would need its own render path and could not be edited before it committed. A component inherits, at no cost:

* **rendering** — the raycaster already draws any component and already honours `ChunkHeader::rotation`, so a rotated stamp is visible exactly as it will land;
* **the transform gizmo**, which already operates on `editor.selectedComponent`;
* **the yellow selection outline**;
* **Sculpt and Paint working on the stamp before it commits**, which is the whole of "edit it how you please" for free;
* **"keep this as its own object"** as a commit path that does nothing at all.

#### Shape

Seven primitives — Box, Sphere, Cylinder, Cone, Wedge, Pyramid, Torus — sized in **voxels**, with the world size shown underneath, for the reason the Paint panel already gives. `Hollow` keeps only a shell of whatever wall thickness is asked for; it is one rule for all seven (inside the shape, and not inside the same shape shrunk by the wall), which is why adding a primitive is one predicate rather than two.

A click drops the shape onto the surface under the cursor, pushed out along that face's normal so it sits *on* the surface rather than half-buried, or `Place distance` down the ray when the click hits nothing — the same fallback the Sculpt brush has, and the only way to put a shape into a component that is still empty.

The stamp takes its **voxel scale from the target**, never from the New Data component's setting. A stamp at a different voxel size makes every merge a resample, however the rotation is snapped. Its resolution is the smallest power of four that holds the largest dimension.

A *generated* shape can be resized after it has been spawned, about the point it is standing on: regenerating a primitive at new dimensions is exact and free. A *lifted* region cannot, because scaling voxels is a resample and there is nothing to resample it from. That is the only place the two sources behave differently.

#### Region

| Selector | Picks |
|----------|-------|
| **Box** | Click one corner voxel, then the opposite one. A live wireframe follows the cursor between the two clicks |
| **Face** | `gatherFaceRegion`, unchanged — the same flat surface Extrude moves |
| **Volume** | The volume fill's traversal, returning its bitset instead of painting it |

Face and Volume take the [scope toggle](#selection-scope) below, unchanged. On a photo-textured scene that toggle is the difference between selecting four voxels and thirty thousand.

A selection is **bounds plus one bit per cell**, not a `vector<ivec3>`: the volume fill already established why, and a million-voxel selection is 128 KB one way and 12 MB the other. It also answers "is this coordinate selected?" in constant time, which the lift and the delete each ask once per voxel.

**Copy** lifts the selection and leaves the source alone; **Cut** lifts it and removes the source in the *same* history entry, which is what makes it different from Copy-then-Delete. **Delete**, **Fill** (recolour with the palette's current entry; never adds a voxel) and **Duplicate** (a copy stepped one bounding box aside) act on the selection without lifting.

A lift builds a fresh Chunk through `queueVoxelAdd` — never `utils::duplicateComponent`, which does not handle Grid components, and a selection routinely lives in one. It is positioned so its lattice *coincides* with the source's, so the stamp appears exactly over the original with no visual jump, and merging it straight back is a byte-identical no-op.

#### Rotation: one toggle, two honest modes

**`Snap 90`**, on by default.

**On.** Rotation is a multiple of 90° about a lattice axis, translation is a whole number of voxels, and the voxel scales match — so the merge is a **1:1 integer remap**. Every source voxel lands on exactly one target cell, nothing aliases, nothing is lost. This is the mode for everything built to the grid.

**Off.** Free rotation, and the merge **rasterises the rotated shape into the target lattice**. This is not a degraded fallback; it is the point of the setting. A wedge meant to sit at 30° in the final geometry, or a tree placed at its own angle so that a dozen copies do not read as a dozen copies, can only be made this way. The panel says so plainly rather than warning about it — a warning would be telling the user their deliberate choice is a mistake.

#### The merge must pull, not push

This is the one part of the stamp that is easy to get wrong in a way that looks almost right.

Forward-mapping each stamp voxel to a target cell (**push**) leaves holes. A rotation is not area-preserving on a lattice, so two source voxels can land in one target cell while a neighbouring cell receives none — and on a hollow shape, whose walls are one or two voxels thick, those gaps perforate the surface. The result is a rotated shape you can see through.

So the merge iterates the **target** cells instead (**pull**): take the stamp's oriented bounding box, walk every target cell inside it, inverse-transform that cell's centre into the stamp's voxel space, and sample. Every target cell gets exactly one answer, so the surface is closed by construction.

The cost is therefore the **volume of the oriented bounding box**, not the stamp's voxel count — which is why that box's volume is the number capped (`STAMP_MAX_MERGE_CELLS`), and why a merge that would exceed it is refused rather than truncated: a fill that stops halfway leaves a smaller fill, but a merge that stops halfway leaves half an object embedded in the scene.

With `Snap 90` on the pull degenerates to the exact integer remap — the inverse transform is a signed axis permutation, and every target cell in the box maps to exactly one source cell — so **one code path serves both modes**. Sampling at cell *centres* is what makes that numerically safe: a centre maps to an integer ± 0.5 in the other lattice, half a cell away from the nearest rounding boundary.

The stamp is read into a dense occupancy-and-colour box once before the walk, so the inner loop is a bit test rather than a tree64 descent. It is read back **from the scene** rather than from the list that built it, so a stamp that has been sculpted or painted while floating merges as it looks — which is the whole point of it being a real component.

#### Committing

| Exit | Does |
|------|------|
| **Merge** (`Enter`) | Writes into the target and releases the stamp |
| **Keep as component** | Clears the floating flag and leaves it in the hierarchy as its own object — often what is actually wanted for a placed pillar or tree |
| **Cancel** (`Esc`) | Releases the stamp, and if it was a *cut*, puts the source voxels back |

`Subtract` mode takes the stamp's volume out of the target instead: spawn a sphere, subtract, and you have a crater. Same walk, `queueVoxelRemove`.

Each merged voxel carries **the stamp voxel's own colour**, not the palette's current entry — the same principle Extrude follows in taking its source voxel's material. A lifted region keeps its pattern.

**The undo record is exactly Extrude's**, and for exactly the same reason. Displaced cells are split two ways: cells that were empty (reverse by removing) and cells that held something (reverse by writing the old contents back). The naive version — "adding fills empty space, so undo empties it again" — is wrong the moment a stamp lands on geometry that was already there, and deletes voxels the merge never created.

#### Two things worth knowing

**The stamp component is reused, not recreated.** The hierarchy's Delete is a soft delete: it unparents the record and renames it `__deleted__`, and nothing reclaims it. A component created and thrown away on every placement would grow `scene.components` without bound over a session of stamping. So one stamp component is kept per resolution and refilled — resolution is fixed for a component's whole life, which is why one reusable stamp cannot serve every size, and there are only ever five of them. A component leaves the pool exactly once: when **Keep as component** hands it to the user.

**A stamp's placement stays out of the undo history.** Aiming a stamp is not an edit to the scene; the merge is. Recording every gizmo frame would leave undo entries pointing at a component that has since been emptied and handed back to the pool, so undoing far enough would slide an invisible stamp around instead of putting geometry back.

### Selection scope

Five things spread a selection outward from the voxel you clicked — the Extrude face gather, both fills, and the Region tool's Face and Volume selectors — and they all answer the same question, so they answer it the same way (`SelectionScope`):

| Scope | Spreads until |
|-------|---------------|
| **Material** | The material changes. This is the tool picking the brick out of a brick-and-mortar wall |
| **Whole face** / **Whole volume** | The geometry runs out. Ignores materials entirely |

It has to be a choice rather than a default, because on a surface of one material the two are identical and on one built from several neither answer is more obviously right. It also depends enormously on how the scene was made, which is not something the tool can infer — and the difference is not subtle:

| Scene | `Material` face | `Whole face` |
|-------|-----------------|--------------|
| StonehillCastle (flat-coloured) | 3,213 voxels | 4,101 |
| Sibenik (photo-textured) | 1 voxel | 35,431 |
| LostEmpire (photo-textured) | 4 voxels | 31,963 |

A voxelisation made from photographic texture gives nearly every voxel its own palette entry, so `Material` selects a handful of voxels and `Whole face` is the only useful setting; on flat-coloured geometry `Material` is the precise one. Neither is the right default for both, which is exactly why it is a toggle.

Extruded voxels carry **their own source voxel's material** up each column, not one colour for the whole face. Under `Material` that is a distinction without a difference, but under `Whole face` it is what stops a patterned surface from flattening to whichever entry happened to be clicked.

The caps (radius 32, cube side 64, fill 4,000,000 voxels) are about a dab staying inside a frame. Every candidate coordinate costs a tree64 descent to find out whether a voxel is there at all, so the *scanned box* — not the painted count — sets the cost: radius 32 is a 65³ box, a quarter of a million descents. Fill needs its own limit for a different reason: it has no bound of its own, and one started on a terrain's ground material would walk the entire component. Its budget counts voxels **of the region**, not coordinates probed — those differ by about a factor of seven, and conflating them is a bug this code has already had (below).

#### Why the volume fill is breadth-first over a bitset

Three details of the traversal are load-bearing, and each was wrong once:

* **The budget counts region voxels, not probes.** The visited set holds every *rejected* neighbour too, roughly six or seven per voxel of the region. Measuring the budget against it stopped a nominal one-million fill at about 160,000 voxels of actual region.
* **Breadth-first, not depth-first.** A stack-based fill runs to the end of one axis before taking its first step along another. Stop one early and the result is not a compact partial region — it is long tendrils with unpainted material between them.
* **Visited is a bitset per chunk, not a hash of packed coordinates.** A chunk's voxels are a dense cube of known size, so a bit each is the natural set: 256³ is 2 MB, allocated only for chunks the fill enters, and a test is an index rather than a hash. The hashing version cost ~40 bytes and a hash per voxel, which is what forced the budget low enough for the first two problems to bite.

Together the first two produced a fill that stopped at a fraction of its region and painted a *striped* subset of it — and the stripes were the traversal order, which is invisible from the outside because a fill's shape is supposed to be arbitrary. None of it reproduces on a small region, which is how it survived the first round of testing.

#### How it reaches the geometry

Paint writes through the engine's existing edit pipeline: `queueVoxelAdd` → `updateScene` → the GPU flush the render loop already performs. The queue works in packed colours rather than slot numbers and `internMaterial` resolves a colour back to a slot on the way in, so painting with the palette's current entry writes that entry's colour and lands back on that entry — the editor never has to reason about slot numbering.

The whole stroke is queued in **one** call. `updateScene` forks and rebuilds every chunk a queue touches, so a thousand voxels queued together cost one rebuild of each chunk they fall in, where a thousand separate calls would cost a thousand.

#### The two coordinate mappings

A pick reports the voxel's coordinate *inside the chunk it hit*; the queue takes coordinates in the component's continuous voxel space; and a brush has to ask about coordinates nobody picked. So both directions are needed:

* `pickToComponentVoxelCoord` — chunk-local → component space, the direction a pick arrives in.
* `componentVoxelToChunk` — back again, the direction a brush asks in.

For a loose Chunk these are the identity. For a Grid they are not: the chunk is one cell, and `applyComponentQueue` buckets an op out to a cell with `floorDiv(position, resolution)` against `grid.originCellCoord`. The two mappings must be exact inverses, and **nothing else in the editor would notice if they were not** — a brush whose reverse map is off paints a plausible-looking region in the wrong cell, which reads as "the tool is weird near chunk edges" rather than as a bug.

### The self-test

`EDITOR_SELFTEST=1` runs three read-only checks after the scene loads. All three exist because the failures they catch are invisible from the outside:

```bash
EDITOR_SELFTEST=1 ./scene_editor ../ScenePreviewer/scenes/StonehillCastle
# SELFTEST paint coords: 50653 probes, 5494 solid, 0 mismatches, 0 unmappable
# SELFTEST sculpt lattice: 1728 probes, 0 lattice mismatches, 0 round-trip failures
# SELFTEST fill: comp=0 slot=0 gathered=1556106 truncated=0 leaks=0
# SELFTEST fill face: comp=0 normal=(-1,0,0) gathered=3213 off-plane=0 outside-volume=0 -> PASS
```

**`runPaintCoordSelfTest`** round-trips both coordinate mappings against the tree64. A Grid's resolution is *not* reliably available from `dataReferences[dataRefID]`: `dataRefID` is `-1` until a component's first edit assigns one (`ensureDataReference`), so on a freshly loaded scene — which is every scene, before anything is painted — the reverse map reported an empty world and every brush found nothing. The resolution is read off the first populated cell instead, the same fallback `ensureDataReference` uses.

**`runSculptLatticeSelfTest`** checks the world lattice against the per-chunk mapping `utils::pickVoxel` inverts to report `worldPosition`, and round-trips world↔voxel. A wrong `coordOrigin` on a downward-expanded Grid fails **here and nowhere else**: every chunk stays individually correct, and the whole component is simply offset by a cell or two from where the tool aims.

**`runFillSelfTest`** checks that a volume fill is **adjacency-closed**: no solid voxel of the seed's material may touch the gathered set without being in it. It seeds on the component's most common material, because the small regions were always correct — that is precisely why the striping survived the first round of testing. It reported 1,147,019 leaks on StonehillCastle before the traversal was fixed, and 0 after. It then gathers a **face** fill from the same seed and checks the two properties that justify the mode existing at all: it never reaches a voxel the volume fill did not, and it never leaves its plane. A face fill that escaped its plane would be a volume fill wearing the safer name — exactly the surprise the split exists to prevent.

Run all three after touching either mapping, the fill traversal, or anything about how a Grid derives its resolution or moves its origin.

#### The sculpt stroke test

`EDITOR_SCULPTTEST=1` is a **separate switch, because this one edits the scene** — in memory, never saved, and undone again through the history before it returns (which also checks that undoing a stroke restores the voxel count exactly). The read-only checks above are cheap to leave on; an editor that edits the scene on startup is not.

It runs a 20-frame stroke with the ray **held still**, which is the sharpest form of the problem: every frame casts the same ray into a scene that now contains the previous frame's dab. Four cases — add and remove, each with the override kept and removed. The control clears `sculptStrokeTouched` between frames, which is precisely the tool with the fix taken out; without it, "the voxels stayed near the surface" would also be the result if the stroke had quietly placed nothing at all.

```bash
EDITOR_SCULPTTEST=1 ./scene_editor ../ScenePreviewer/scenes/StonehillCastle
# SCULPTTEST comp=0 surface=398.43 voxelSize=1.000 frames=20 tolerance=5.00
# SCULPTTEST   add,    override: 99 voxels, span [396.0, 401.5], ray now 398.43 -> PASS
# SCULPTTEST   add,    control : 2124 voxels, span [333.9, 401.5], ray now 333.70 -> PASS
# SCULPTTEST   remove, override: 38 voxels, span [396.3, 401.1], ray now 398.43 -> PASS
# SCULPTTEST   remove, control : 38 voxels, span [396.3, 401.1], ray now nothing -> PASS
# SCULPTTEST   controls must drift and overrides must hold; undo back to 1727740 -> PASS
```

The control climbs 64.7 units toward the camera when adding, and when removing it bores clean out of the scene — `ray now nothing`. On Sibenik, which has interior geometry to eat, the removal control tunnels 31 units deeper instead.

`runExtrudeSelfTest` runs alongside it, checking three things that all fail silently:

```bash
# EXTRUDETEST comp=0 face=3213 voxels normal=(-1,0,0)
# EXTRUDETEST   face is one plane/material/surface: 0 off-plane, 0 wrong material, 0 buried -> PASS
# EXTRUDETEST   depth 1 adds one per face voxel: 1727740 -> 1730953 (expected 1730953) -> PASS
# EXTRUDETEST   reversible mid-drag: out to 5 (1743805) and back (1727740), in to -3 (1718101) and back (1727740) -> PASS
# EXTRUDETEST   undo of a depth-3 drag: 1737379 -> 1727740 (baseline 1727740) -> PASS
# EXTRUDETEST   extruded over an existing voxel, then retracted: survived=true colour kept=true -> PASS
```

1. **The face is a face.** A gather that leaked through the material test or out of the plane still produces a plausible-looking extrusion — of the wrong region.
2. **A drag is reversible in place**, before any undo is involved.
3. **Depth one is exact.** Every voxel of a face has, by definition, an empty cell in front of it, so one layer out adds precisely one voxel per face voxel. Any other number means the face and the layer disagree about which cells they cover.

The last line **builds its case on purpose**: no natural face in any shipped scene extrudes into occupied space (the counts show every layer landing in empty air), so the test parks a voxel of another colour two layers out and checks it survives the round trip with its own material intact. Without it the displacement path would be untested — which matters, because that defect was found by reading the retract path, not by watching it fail.

**It asserts on where the ray still thinks the surface is, not on how far the placed voxels reach**, and the difference matters: the two failures do not look alike from the geometry's side. Climbing piles voxels toward the camera and shows up in their extent; tunnelling only does if there is something deeper to eat, and a voxelised surface is usually a *shell*. The first version of this test measured extent and passed the removal control after it had bored straight through the wall. Both failures share exactly one signature — the ray stopped agreeing with where the stroke started — and that holds whatever the object is made of.

`runSculptOperatorSelfTest` runs under it too, and unlike the others it does **not** trust whatever scene happens to be open: it builds a two-layer slab in empty space with a one-voxel spike on it, a one-voxel dent in it, and a voxel floating clear of both. On a real scene "did it get smoother" has no answer; on a shape built for the purpose every assertion is exact. One layer would not do — a lone layer is not stable under a majority filter (its cells see only 9 of 27) and the test would be measuring that instead.

```bash
# OPERATORTEST comp=0 slab=13x13 at (-254,-254,-254) radius=5.0 (built slab=true spike=true dent=true noise=true)
# OPERATORTEST   smooth at full strength removes a spike, fills a dent and clears noise: ... -> PASS
# OPERATORTEST   smooth at strength 0 (cutoff 6) takes only the noise, however long it is held: ... -> PASS
# OPERATORTEST   smooth at strength 2 (5x5x5) takes a two-layer slab that strength 1 cannot: true -> PASS
# OPERATORTEST   smooth leaves a flat surface alone: true -> PASS
# OPERATORTEST   bump pulls one layer and pushes it back: flat=true pulled=true pushed=true -> PASS
# OPERATORTEST   blend softens the rim: tallest step 3 at blend 0, 2 at the default 1, 1 at 4 -> PASS
# OPERATORTEST   smooth tick at radius 10: 0.35 ms at strength 1, 1.00 ms at 3 (budget 100 ms) -> PASS
```

The three that matter most are the ones a bug would leave looking plausible. **Stable on a flat surface** is the difference between a smooth brush and a delete brush — a majority filter that drifted would eat a wall just for being pointed at. **However long it is held** and **that strength 1 cannot** are the two halves of what makes [the strength setting](#smooths-strength) a strength rather than a delay — the first that a weak pass is selective, the second that a wide one reaches a shape no amount of holding does. And the **rim step** is measured, not eyeballed: the tallest single-cell step anywhere around the bump's edge, which is the thing that reads as a stamped sphere.

`runPaintStrokeSelfTest` runs under the same switch, for the same reason — it paints and then undoes itself. It presses at one point on a surface, samples once more a couple of dozen voxels along it, and releases, then asserts the four things a stroke has to get right:

```bash
# SELFTEST paint stroke: comp=0 sweep=23 voxels painted=24 (two dabs alone=2) entries=1 duplicates=0 wrong-after-paint=0 wrong-after-undo=0
# SELFTEST paint stroke: one entry per stroke PASS | no repeats PASS | colour applied PASS | undo restored PASS | gap filled PASS
```

Only the first two of those can be seen from a single frame. **One entry per stroke** is what makes a sweep one press of `Ctrl+Z` rather than a dozen. **No repeats** is the within-frame overlap being dropped. And **gap filled** is the point of the whole stroke, asserted against the strongest control available: the same two dab centres collected on the untouched scene with nothing between them. Two dabs paint two voxels; the stroke paints the twenty-four along the path. Without interpolation those numbers are equal — which is exactly the dotted line clicking repeatedly already gave.

#### The stamp

`STAMPTEST=1` has its own switch because it builds **its own components** rather than editing the loaded scene, so it is the same test on every scene and leaves no mark on the user's. A merge that is off by one cell produces a perfectly plausible result in the wrong place — the same failure class the paint-coordinate test guards, and one that reads as "the tool is a bit weird" rather than as a bug.

```bash
STAMPTEST=1 ./scene_editor ../ScenePreviewer/scenes/StonehillCastle
# STAMPTEST: baseline=216 voxels, offset strays=0, wrong=0, shell=3904 voxels, sealed cells=7999
# STAMPTEST: lift/merge identity PASS | offset merge PASS | undo restores PASS |
#            cut+cancel restores PASS | four turns identity PASS | hollow at 37 sealed PASS
```

The baseline is a 6³ block in which **every voxel is a different colour**, so a merge that lands in the right place but the wrong orientation is still caught — a uniformly coloured block would pass a transposed remap without a murmur. (Six a side, not eight: a distinct colour per voxel is a distinct palette entry per voxel, and material IDs are `uint8_t`. 6³ is 216 entries and fits; 8³ is 512 and does not.)

* **lift/merge identity** — lift a copy and merge it straight back at zero offset. Byte-identical, or the lattice the lift builds its stamp on does not coincide with the one it came from.
* **offset merge** — the same block eleven voxels over: exactly the baseline plus a translated copy, no strays.
* **undo restores** — the merge's record has to reverse the cells it filled *and* restore the ones it displaced.
* **cut+cancel restores** — a cut removes exactly the selection, and cancelling puts it back exactly.
* **four turns identity** — four quarter-turns compose to the identity transform. A rotation that is nearly-but-not-quite a lattice rotation accumulates, and four is where it first becomes visible.
* **hollow at 37 sealed** — the [pull-rasterisation](#the-merge-must-pull-not-push) test, and the one that fails loudly if anyone ever reimplements the merge as a forward map. A hollow 20³ box with a two-voxel wall is merged at 37° about an off-axis axis, then the empty space *around* it is flooded from a corner well outside. The assertion is crisp: the flood must not reach the middle. One hole anywhere in the surface and it does.

The shell count is worth reading too — 3,904 voxels is exactly 20³ − 16³, so the rotated merge reproduced the shape's voxel count to the voxel.

#### Undo

Undo repaints each voxel with the colour it had — a per-voxel list, since a sphere or a cube can span several materials. Only voxels whose colour would actually change are collected, so a brush dragged over a wall it has already painted queues nothing and the undo step is the set of voxels that really moved. A whole stroke is **one** entry, so undoing puts the scene back to where the button went down.

The coordinate list is shared between the undo and redo closures rather than copied into each; a fill can run to a million voxels, and three copies of that list would be 36 MB the history is charged for and has to carry. `record.memoryCost` reports the real figure, so a big fill counts properly against the log's 512 MB cap.

## The Library

The left column's second half: a persistent browser over the folders scenes and assets live in. Folders holding a `compose.json` are tinted and marked `[scene]`; selecting one lists its top-level components, read with `parseComposeJson` — **no geometry is read and nothing reaches the GPU**, so browsing a 3 GB scene costs a file read.

It is deliberately separate from the Scene Hierarchy above it rather than being extra roots in the same tree. The hierarchy answers "what is in my world" and its rows are renamed, reparented and deleted; the library answers "what could be" and its rows are searched and opened. One tree with two sets of operations that mean different things on rows that look alike is worse than two panels.

**Import is not offered yet, and the button says so.** Copying a component between scenes means appending another `Scene`'s geometry blobs, palette and chunk records into this one and remapping every handle they carry — engine work that does not exist. Loading a scene works today. This is also why multi-scene is a *browser* rather than co-resident scenes: [loading is two-phase](#loading-is-two-phase) precisely because holding two scenes' textures at once killed a Vulkan driver, and "open a second scene to borrow from" would do exactly that on purpose.

The Inspector's local Position, Rotation, and Scale are editable — drag to change, undoable, live in the viewport as you drag. Rotation is shown and edited as Euler degrees (a quaternion has no natural "drag this number" widget); World transform stays read-only, shown for reference. "Reset transform" clears all three to identity.

Editing Scale required a real engine fix, not just UI: `ComponentRecord::localScale` existed but `rebakeSubtree` (`scene_query.cpp`) never applied it to a Chunk's `header.scale` or a Grid's `cellSize` — and separately, extracting rotation from a matrix that still had scale baked into its columns would have silently produced a wrong quaternion the moment scale stopped being 1.0. Fixed by decomposing (position, rotation, uniform scale) properly — extract scale via column length, normalize before `quat_cast` — the same technique `loadComposeFromDisk` already used when a compose.json leaf's transform is first decomposed. `Chunk::nativeScale` / `SceneGrid::nativeCellSize` (new, runtime-only — `ChunkHeader` itself is disk-persisted and untouched) hold the transform-independent size so repeated edits don't compound. Verified against the `editing_p1` test driver: identical pre-existing failures before/after (none touch transforms), plus three new checks (2x scale, 0.5x, restore) all pass.

## Selecting a component

There are three ways in — clicking a voxel in the viewport (Select or Move tool), clicking a node in the Scene Hierarchy, and clicking an ancestor in the breadcrumb — and all three land on the same selection, which every other panel follows.

Whichever route it came by, the selection outlines every `.data` box it covers in **yellow** in the Viewport — the node itself if it's a Chunk or Grid, every occupied cell if it's a Grid, or every leaf beneath it if it's an Asset folder (`main.cpp`'s `collectLeafChunks`). The Inspector's "Boxes" count is the same set.

The outline is drawn in screen space: each chunk's OBB corners (`header.position + header.rotation * localOffset`, the same convention `fetchVoxelColor` and `utils::pickVoxel` use) are projected with `worldToViewportPixel`, the exact inverse of the shader's `rayStartDirection` — so the box lines up with the geometry the raymarcher actually drew, at any camera angle. A corner behind the camera drops its edges rather than clipping them; adequate for a selection hint, wrong for anything claiming precision.

## Palette editing

The Palette panel shows one component's materials as a grid of colour swatches with the number of voxels using each underneath — a palette is a set of colours, so the colours are what the grid shows, and names live in the detail pane below. Selecting a swatch opens it:

* **Colour** — a picker that recolours live. Dragging it repaints every voxel using that slot as you drag, because a recolour cannot move a palette offset: it takes `graphics::updatePaletteEntry`, a single texel write, rather than a full palette rebuild that would also dirty every chunk header.
* **Voxels** — how many voxels use the entry, what share of the component that is, and which chunks they are in. Clicking a chunk moves the camera to it.

**Add entry** appends a white material. **Remove entry** deletes one — and because slot numbers *are* the data (every voxel stores its slot as a byte), removing renumbers every slot above it and rewrites the material byte of every voxel that referenced them. When the entry is in use, a dialog asks which entry its voxels should become. Geometry shared with another component is copy-on-written first, so the other component's colours are untouched.

The counts are true voxel counts, not material-byte counts: a *uniform* leaf stores one byte for up to 64 voxels (~96% of leaves on terrain), so counting the `materialIDs` array would under-report by an order of magnitude. `utils::countMaterialUsage` walks the tree instead.

### The eyedropper

The dropper button in the Palette panel's header (and in the Tool panel's material row) arms a pick; the next click in the viewport selects the palette entry of the voxel under the cursor (Esc cancels). Under **Paint** and **Sculpt**, `Alt`+click does the same thing without arming anything first — the standard modifier in every paint program, and the reason neither tool needs the armed mode. Both place the palette's current entry, so "use that colour" has to be reachable without leaving the tool. The ray is cast on the **CPU** — `utils::pickVoxel` sorts chunks by their bounding boxes and walks the winner voxel by voxel, reading materials straight out of the tree64 with `utils::queryVoxelMaterial`.

That is deliberate. The GPU knows the answer, but reading it back stalls the pipeline and needs the viewport renderer to keep a G-buffer it otherwise has no use for. A CPU pick costs one ray on a click, needs nothing from the renderer, and hands back the exact voxel coordinate and chunk — which is what voxel editing will need next, not just a colour.

`rayDirectionThroughImage` is a line-for-line port of `rayStartDirection` in `pjv_utils_DDA.sc`. If that shader's ray generation changes, this must change with it or picks land beside what was clicked.

## Undo

Every edit is recorded in `edit_history.{h,cpp}` as a pair of closures — one that reverses it, one that reapplies it — rather than as a struct in a central switch. The operations coming next (voxel add, erase, transforms) reverse in completely different ways, and each knows how to reverse itself better than a central authority would.

* `Ctrl+Z` / `Ctrl+Shift+Z`, and the Edit menu, which names the step it will undo. (`Ctrl+Y` was a second Redo binding until the Region tool took the letter.)
* The History panel lists every edit; clicking one walks the scene to the state just after it.
* A colour drag is **one** undo step: consecutive edits to the same target within a second coalesce, keeping the colour the drag started from.
* Removing a palette entry is undone from a **snapshot** (the palette, the affected blobs' material bytes, and which blob each chunk pointed at). The operation renumbers slots and rewrites voxels, so there is no inverse to compute — only a record to restore. The log is capped at 512 MB and drops its oldest steps past that.

## Loading a scene

File ▸ Load Scene… opens a directory browser. A folder holding a `compose.json` is a scene and is tinted and marked `[scene]`; anything else is a step on the way to one. Click to select, double-click to commit — opening an ordinary folder, or loading a scene one. The path field at the top takes a typed path on Enter.

Any Compose folder works, not just the bundled ones:

```
../ScenePreviewer/scenes/Sibenik        # a bundled scene
../MeshVoxelizer/trees/Oak_Leav         # a 64^3 asset
../PathTracer/SponzaScene/              # the Sponza atrium
```

## How the scene gets into a dock panel

The editor's renderer (`editorRenderer/`) is the previewer's three passes with one change in `render.json`: the display pass writes to `frameBufferOutputID: 3` rather than `-1`. FBO 3 is a new `RGBA8` texture, `viewportColor`, and that texture is what `ImGui::Image` draws in the Viewport panel.

Three consequences shape `main.cpp`'s render loop, which is written out rather than calling `renderConstructedRenderer`:

* **The back buffer and the scene's targets have different sizes.** The back buffer follows the OS window (`bgfx::reset`); the scene's textures follow the Viewport panel. The engine's `resizeFramebuffersAndTheirTexturesIfNeeded` couples the two — it calls `bgfx::reset` with the size it is given — so the editor has its own `resizeViewportTargets`, which is that function's texture half without the reset, and which destroys the framebuffer handles it replaces (the editor resizes on every frame of a splitter drag; leaked handles would exhaust bgfx's framebuffer pool in seconds).
* **The panel's size is applied one frame late, deliberately.** The interface hands ImGui the viewport texture's handle, so the handle must not change after the interface is built. Resizing happens at the top of the frame, before anything reads it, which means the texture ImGui is given is the one that frame's scene passes render into.
* **View order does the compositing.** The scene passes take bgfx views 0–2, ImGui takes view 200, and bgfx submits views in ascending ID order — so the scene is finished before the interface samples it, regardless of the order the C++ submits them in.

Because the scene is rendered at exactly the panel's resolution and the `windowRes` uniform is set to the same, there is no letterboxing or aspect distortion in the panel, and the accumulation pass resets whenever the panel is resized (its history is the wrong size).

## The ImGui backend

Dear ImGui ships a GLFW **platform** backend (used here as-is) but no bgfx **renderer** backend — bgfx keeps its ImGui integration inside its example framework, which drags in bx allocators, the entry-point layer, and its own embedded shaders. `imgui_impl_bgfx.{h,cpp}` is the same job done against the engine's primitives: shaders through `graphics::loadShader`, geometry through bgfx transient buffers, one bgfx view.

It implements ImGui 1.92's `ImTextureData` protocol (`ImGuiBackendFlags_RendererHasTextures`), so ImGui creates, updates, and destroys its own atlases through `ImDrawData::Textures` rather than the backend building a font texture up front.

The two ImGui shaders live in their own directory, `editorRenderer/imguiShaders/`, because of the `varying.def.sc` beside them: `ImDrawVert` fixes ImGui's vertex as a 2D position, one UV, and a packed RGBA byte colour, which cannot share the viewport renderer's varying definition where `a_position` is the vec3 of a fullscreen quad. Note that `varying.def.sc` files carry **no comments** — shaderc's parser for them silently drops the declaration that follows one.

## Files

```
main.cpp                              The editor: state, panels, camera, render loop
imgui_impl_bgfx.{h,cpp}               Dear ImGui renderer backend for bgfx
compEditor.sh                         Compiles both shader directories
editorRenderer/render.json            Three passes; display targets FBO 3
editorRenderer/resources.json         previewColor, accumColor, viewportColor + their FBOs
editorRenderer/editorShaders/         albedo, accumulate, display (+ fullscreen quad VS)
editorRenderer/imguiShaders/          vs_imgui, imgui (+ their own varying.def.sc)
```

Scenes are not bundled: the editor points at `../ScenePreviewer/scenes/`, whose `.data` files are tens of megabytes and are already in the repository once.

## ProjectV Features Used

* `utils::loadComposeFromDisk` — Compose scene folder → `Scene`
* `Scene::components` (name, kind, parent/children, local transform) — the hierarchy tree
* `utils::getComponentPath`, `getComponentWorldPosition`, `getComponentVoxelCount` — the inspector panel
* `graphics::createTexturesForScene` / `destroyGPUData` — per-load GPU upload and teardown
* `graphics::updatePaletteEntry` — single-texel palette writes for live recolouring
* `utils::addMaterial` / `setMaterialColor` / `setMaterialName` / `removeMaterial` — palette editing
* `utils::countMaterialUsage` / `findMaterialChunks` — voxels per slot, and which chunks they are in
* `utils::pickVoxel` / `queryVoxelMaterial` / `rayDirectionThroughImage` — CPU picking, for click-to-select, the eyedropper, and every frame of a sculpt stroke (via `VoxelSolidityOverride`)
* `utils::queueVoxelAdd` / `queueVoxelRemove` / `updateScene` + `graphics::flushSceneUpdates` — the write path every editing tool uses
* `utils::addComponent` / `isValidChunkResolution` — creating the stamp component a Shape or Region placement floats in
* `utils::parseComposeJson` — the Library's scene contents, read without loading any geometry
* `graphics::loadRendererSpecification` / `constructRendererSpecification` — the JSON-described viewport renderer
* `graphics::performRenderPasses`, `updateUniforms`, `setUniformToValue` — the render loop, driven by hand
* `graphics::getTextureAttachments` — rebuilding framebuffers on viewport resize
* `core::ecs` — Startup/Update/Render/**Shutdown** stages and global resources

## Third-Party Dependencies

* **Dear ImGui** (docking branch) — `external/imgui`, a submodule of the *engine*, not of this example: it is intended to become part of the engine proper. Nothing in `lib/` links against it yet, so the example compiles the five ImGui translation units it needs directly.
* **bgfx / bx / bimg**, **GLFW** — as every windowed example.

## Loading is two-phase

Releasing the old scene and building the new one in the same frame keeps both resident: bgfx frees a destroyed texture only after the frames that might still reference it have been rendered. A 3.2 GB scene reloaded that way asks an 8 GB card for 6.4 GB, and the Vulkan driver dies mid-submit. So a load releases, lets eight frames pass, then builds — the viewport shows its empty state for those frames. A bad path is rejected *before* anything is torn down.

## Selecting a component: the yellow outline

Clicking a node in the Scene Hierarchy outlines every `.data` box it covers in yellow in the Viewport — the node itself if it's a Chunk or Grid, every occupied cell if it's a Grid, or every leaf beneath it if it's an Asset folder (`main.cpp`'s `collectLeafChunks`). The Inspector's "Boxes" count is the same set.

Implementing it was mostly a matter of getting the projection math exactly right rather than anything algorithmically hard: each chunk's OBB corners (`header.position + header.rotation * localOffset`, the convention `fetchVoxelColor` and `pickVoxel` already use) are projected with `worldToViewportPixel`, which is the algebraic inverse of the shader's `rayStartDirection` — build the same camera basis (right/up/forward from yaw+pitch), but instead of turning a screen UV into a ray direction, turn a world-space offset into a screen UV. No new render pass, no GPU work: it's ImGui line-drawing on top of the already-rendered frame, using the same per-frame camera state the render loop already tracks. The one bug it caught along the way was unrelated to the outline itself — a stale "last item" in the hierarchy's click handler (see git history) that meant clicking a tree node never actually selected it.

## What's Next

* **A brush preview in the viewport**, and a highlight of the face Extrude would take. Sculpting commits on the first frame of a drag and shows the result; what it cannot show is what the *next* press would affect. Both are already computed — the brush cell by `processSculptSample`, the face by `gatherFaceRegion` — so this is an outline to draw, not a calculation to add. The face highlight matters most: how far a face spreads depends on the scene's materials, and right now the only way to find out is to drag it.
* **Rotating the cube brush.** The box is axis-aligned in the component's voxel space. The Tool panel used to advertise `WASDQE` rotation, which never existed and has been removed rather than left as a promise; a rotated box means rasterising an oriented box into the grid rather than scanning an axis-aligned one.
* **Undo of an additive stroke leaves empty cells behind.** Removing the voxels does not remove a Grid cell the stroke caused to be created — an empty chunk, drawing nothing and costing a header row. Harmless, and the next save drops it, but a cell reaper would be tidier.
* **Cross-scene import**, which is what the Library's disabled Import button is waiting on: appending another `Scene`'s geometry blobs, palette and chunk records into the open one with every handle remapped. `duplicateComponent` is the within-scene version of the same walk and does not yet handle Grid components either.
* **Scaling a lifted stamp.** The gizmo has no scale handles, and scaling voxels is a resample rather than an edit. A *generated* shape resizes by regenerating, which is exact and free; a lifted region does not resize at all. A resampling lift is the missing piece, and it is a different operation from everything the merge does today.
* **Repointing a stamp at a different target.** The target is fixed when the stamp is created, because a stamp is built at the target's voxel scale and moving it to a component with a different one would make the merge a resample — the same problem as scaling, arriving from the other direction.
* **Saving**: Compose write-back for an edited scene.
