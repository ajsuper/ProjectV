# Scene Composition — Booleans and Snapping Between Finished `.data`s

**Stages 1–3 built. Stage 4 designed, not built.** See
[What the build settled differently](#what-the-build-settled-differently) for the two places the
build had to depart from this plan.

The Place → arrange → merge → sculpt loop inside one asset works, and this plan changes none of it.
What this plan is about is the step *after* that loop: you have two finished `.data`s and you want to
compose them — subtract one from the other, or simply stand them next to each other so they line up.
Both are currently harder than the first loop, and for reasons that are accidents rather than
decisions.

The report this comes from, stated as the user stated it:

> I merged 2 wedges to create a roof. I had one data that was the roof, perfect. Then I duplicated
> it, and shifted it down so I could subtract it from itself to be just left with 2 thin ramps
> instead of the whole wedges. A simple idea. But: I can move these completely freely in the
> scene, they do not actually line up with the other objects in our scene, and I could not merge it
> as subtraction no matter what I did. Using add to create shapes, and merging them into one data,
> and then using sculpt tools on them is working great and exactly as wanted. But actually composing
> the scene when you have multiple `.data`s gets messy and confusing.

Related: [`assembly_system.md`](assembly_system.md) for how the fold works,
[`asset_model.md`](asset_model.md) for what owns it. Neither is superseded — this is the third
document in that line, and it changes one gate and one coordinate frame.

- [Three causes, and none of them is the fold](#three-causes-and-none-of-them-is-the-fold)
- [Cause 1: `Part`-ness is a gate it was never meant to be](#cause-1-part-ness-is-a-gate-it-was-never-meant-to-be)
- [Cause 2: a merged row is `Place`, and `Place` rows do not fold](#cause-2-a-merged-row-is-place-and-place-rows-do-not-fold)
- [Cause 3: there is no lattice above an asset](#cause-3-there-is-no-lattice-above-an-asset)
- [Snapping is a property of the object, not of the editor](#snapping-is-a-property-of-the-object-not-of-the-editor)
- [Staging](#staging)
- [Hazards](#hazards)

---

## Three causes, and none of them is the fold

The fold is correct. Every symptom above traces to something around it.

| Symptom | Cause |
|---------|-------|
| A merged `.data` moves freely, ignoring Snap 90 | 1 |
| "That stack resolves to nothing" on any subtract between two merged `.data`s | 1 |
| Live preview shows both objects instead of the subtraction | 2 |
| Plain **Merge to one Data** merges only the subtract row | 2 |
| Objects in different assets do not line up with each other | 3 |
| Root-level components never snap at all | 3 |

---

## Cause 1: `Part`-ness is a gate it was never meant to be

`ensurePart` (`main.cpp`) has exactly one call site: inside `adopt()` in `syncResolves`. `adopt`
early-returns when the node already has a resolve, so it runs **once per resolve, at creation**, over
whatever children existed at that moment.

`mergeContentsToData` creates its output with a bare `addComponent` and never calls `ensurePart`.
The asset stays open, so its resolve is never retired and never re-adopted, so the merged component
**never acquires a `Part` record for the rest of the session**. The same hole exists in the Assets
panel's `New Data`, and `duplicateComponentInEditor` only copies a `Part` if the source had one — so
a duplicate of a merged `.data` inherits the hole.

Two consumers treat "has a `Part` record" as if it meant something:

- `latticeSnapVoxel` opens with `if (!findPart(editor, component)) return 0.0f;`, so
  `snapTransformToLattice` early-returns and the component free-floats.
- `foldChildren` does `Part* part = findPart(editor, child); if (!part) continue;`, so the component
  contributes nothing to the fold.

Neither of those is what the gate was for. A `Part` is an **editor-side cache** — the primitive a
component was rasterised from, and the cells it last folded to. Its own doc comment says as much.
Membership in a stack is not a cache property; it is *being a child of a node with a lattice*, and
both call sites already test that on the very next line.

The user's repro, traced:

1. Two wedges placed → both registered as `Part`s by the Place tool.
2. **Merge to one Data** → `baked` created, no `ensurePart`. Only child, so its op is set to `None`.
3. Duplicate → source has no `Part`, so neither does the copy.
4. Set the copy's op to `Subtract` → `nodeHasStack` is true, the resolve stays.
5. Live fold: roof is `None` → skipped; copy has no `Part` → skipped. Empty. `rebuildResult`
   destroys the result and puts the sources back, which reads as *nothing happened*.
6. **Merge to one Data** with nothing picked: whole-stack skips `None`, leaving only the copy, which
   has no `Part` → empty → *"That stack resolves to nothing - check the ops on the rows."*
7. Shift-pick both rows — the correct gesture, since `placeAsUnion` promotes `Place` to `Union` — and
   it fails identically, because the `Part` hole is upstream of the promotion.

Every route fails, which is exactly what "no matter what I did" describes.

**The fix.** Stop gating on `Part`-ness.

- `foldChildren` calls `ensurePart` where it called `findPart`. Cheap when the record exists;
  computes content bounds once and caches when it does not.
- `latticeSnapVoxel` drops its `findPart` line entirely.
- `ensurePart` takes `const projv::Scene&`. It only reads.

This is stage 1 and it is about five lines. It fixes both reported symptoms on its own.

---

## Cause 2: a merged row is `Place`, and `Place` rows do not fold

`mergeContentsToData` sets the merged row's op to `None` when it is the asset's only child, and the
reasoning is sound: a finished asset holds one placed `.data`, no ops, resolve retired. But
`foldChildren` skips `None` rows for the live resolve, so the *next* thing you do to that asset —
add a row that subtracts from it — composes against an empty accumulator.

This is not a bug in either piece. `Place` genuinely means "not composition": a placed row survives a
bake as its own component and may sit at its own voxel scale, and the fold is right to leave it
alone. The gap is that the editor lets you author a stack whose only folding row is a `Subtract`,
which can only ever resolve to nothing, and says nothing about it.

**The fix is an authoring assist, not a semantic change.** When a row's op is set to `Subtract` or
`Intersect` and no row above it folds, promote the nearest row above from `Place` to `Union`, under
the same undo record, and say so in the status line.

Deliberately *not* fixed by making the fold treat a leading `None` as `Union`. That would make the
editor's picture disagree with the file: `saveComposeToDisk` writes the row as `none`, and any other
loader would read the placement rather than the fold. The promotion changes the document, so the
document stays the truth.

---

## Cause 3: there is no lattice above an asset

`snapTransformToLattice` quantizes `localPosition` against **the owning resolve's** lattice, which is
`latticeAtNode(resolve.node, resolve.voxelScale)` — the asset node's own world frame.

Two consequences, and the second is the one that makes scene composition messy:

1. A component whose parent has no resolve never snaps. Every root-level component in the document
   is in this position, because the document root is not an `Asset` node and `syncResolves` only ever
   adopts `Asset`s. So the top level of a scene — the level at which you actually compose — has no
   snapping at all.
2. Each asset carries its own lattice *phase*. Two objects in two different assets can each be
   perfectly snapped in their own frame and still be half a voxel out of step with each other,
   because nothing requires the two asset nodes to be in phase.

**The fix: one document lattice, and snap in world space against it.** `snapTransformToLattice`
takes the component's world transform, quantizes position onto a single document-wide lattice —
origin at the document root, voxel size `editor.snapVoxel`, defaulting to the finest `voxelScale`
present in the document — quantizes rotation to 90° about world axes, and converts back to local
through the inverse parent world matrix.

For an asset that is itself on the document lattice at the same scale, this produces today's numbers
exactly, so the working loop does not regress. What changes is that root-level objects snap, and
objects in different assets stay in phase.

The `Snap 90` checkbox becomes **Off / Voxel / N voxels** plus a separate 90°-rotation toggle. The
step is the part the user asked for directly — *the snapping requirements vary wildly* — and it is
the difference between detailing at one voxel and placing modular pieces at 16, 32 or 64.

---

## Snapping is a property of the object, not of the editor

`EditorState::snap90`'s comment already makes the case for free placement, and makes it well:

> A wedge meant to sit at 30 degrees, or a tree placed at its own angle so a dozen copies do not read
> as a dozen copies, can only be made this way.

That is right, and it is why the setting exists. What is wrong is that it is **global**, because
`snapTransformToLattice` runs inside `applyComponentTransform` — the funnel *every* transform goes
down: the gizmo, the arrow keys, the Inspector's numeric fields, and the undo and redo of all three.

The funnel's own comment says snapping is idempotent so replaying a record is not a second nudge.
True — but only while the setting has not changed. So: turn snap off, place the tree at 30°, turn
snap back on to place the next wall. Now nudge that tree one voxel with an arrow key, or undo
something that touched it, and it is rounded onto the lattice **and straightened to the nearest 90°**.
The pose is destroyed silently, long after the decision that made it, by an action that had nothing
to do with rotation.

Generalising snapping (cause 3) without fixing this makes it worse, because more of the scene comes
under the funnel's authority. The two have to land together.

**Three layers, and the middle one is the durable one.**

| Layer | Mechanism | Lasts |
|-------|-----------|-------|
| Gesture | Hold **Alt** while dragging to suppress snapping. | The drag — but see below |
| Object | A `Free` flag on the component. `latticeSnapVoxel` consults it and returns 0. | Until cleared |
| Default | The global control governs what *new* placements get, and nothing else. | New objects |

Alt is unused in the editor today — Ctrl is the shortcut modifier and Shift picks rows. Hold-to-
override is the Blender/Unity/Unreal convention and it is non-modal, so there is no state to forget
to restore.

The object flag needs two things to not become a hidden mode: a marker in the contents row, the way
`op` has a glyph, so a free object is legible at a glance; and a **Snap to grid** button in the
Inspector, since the flag otherwise makes the lattice unreachable for that object.

**Persistence.** Editor-side to start. It has to survive a save eventually, and the honest home is an
optional `compose.json` field (`"snap": false`, default true) — `mutability` is already a
non-geometry authoring-policy field in that schema, so the precedent exists, and unknown fields are
ignorable, so it reinterprets nothing already on disk.

**A default worth having rather than a rule.** Free rotation is not free in the fold: the pull walks
the oriented bounding box's *volume* rather than the part's voxel count, which is why
`ASSEMBLY_MAX_BAKE_CELLS` refuses with *"Turn Snap 90 on, or bake it in fewer parts."* So a 30° wedge
is cheap as a **placed** object and expensive as a **folding** one. Defaulting `op == None` rows to
free-eligible and folding rows to snapped puts the cost where the user is not paying it, without
anyone choosing per object most of the time. The flag overrides either way.

---

## What the build settled differently

**An Alt-drag cannot be gesture-only. It has to mark the component free.**

The table above says the gesture layer lasts for the drag and no longer, and that is wrong for one
reason the plan missed: the drag records its own undo entry, and that entry holds an *off-grid*
value. Redo it with Alt not held and the funnel snaps the value on the way past — the pose the drag
existed to create is destroyed by its own history, one Ctrl+Y later.

So an Alt-drag suppresses snapping *and* sets `freeComponents` on the dragged component, both under
the same undo record, with the pre-drag flag latched in `gizmoDragStartFree` for the same reason the
anchor is latched: every frame after the first would otherwise read the value this drag just wrote
and record an undo that cannot clear it.

This makes the gesture a way of *declaring* an object free rather than a way of temporarily escaping
the grid, which is a larger claim than "hold Alt". Two things keep it from being a mode the object
silently falls into: the contents row carries a `~free` marker beside the op glyph, and the Inspector
shows **Snap to grid** (which clears the flag and rounds it) or **Place freely** (which sets it
without a drag).

Two smaller findings:

- **The mass "pull everything onto the lattice" had to stop being a side effect of the checkbox.**
  It used to fire when `Snap 90` was ticked on. Now that freedom is per component, that would
  silently override every deliberate pose in the asset the moment someone toggled a setting. It is
  its own button — *Pull contents onto the grid* — which also clears the flag, because asking for it
  is a different act from enabling snapping.
- **The self-test harness was leaking `snap90`.** Test 8 sets it false for the 37-degree hollow
  sphere and never restored it, so every test after it ran unsnapped. The whole snap block is now
  saved and restored with the rest of the harness state.

**Setting a row's op was not undoable at all, so "under the same undo record" had nothing to join.**

Stage 3 says the promotion lands under the same undo record as the op change that triggered it. Both
op controls wrote `record.op` straight into the scene and told `editor.history` nothing, so there was
no record to be under. Survivable while an op was the only thing changing — but a promotion that
outlived the Ctrl+Z of the op that caused it would leave the stack folding a row nobody asked to
fold, and it would be a row *above* the one the user last touched. So `setComponentOpInEditor` records
the pair as one entry, which makes an op change undoable for the first time.

Two smaller findings, both about what cause 2 actually does:

- **The subtract row seeds; it does not compose against an empty accumulator.** Cause 2 says the next
  row "composes against an empty accumulator". It does not — `foldChildren` seeds on the first
  *contributing* row whatever its op says (the deliberate rule hazard 3 is about), so the `Subtract`
  row becomes the seed and the fold resolves to the subtracting body itself. That is why the reported
  symptom is *both objects* rather than nothing: the placed original drawn as a placement, the copy
  drawn as the result. The fix is unchanged; the number the test asserts against is 512, not 0.
- **The change has to be deferred out of the Assets panel row loop**, which the panel's existing
  comment does not cover: it defers mutations that *append* to `scene.components`, and this one
  appends nothing. It promotes a row *earlier* in the list — one already drawn this frame — so
  applying it mid-walk shows the pre-promotion glyph beside a post-promotion fold for exactly the
  frame in which the user is looking for that glyph.

---

## Staging

**Stage 1 — the `Part` gate.** `foldChildren` → `ensurePart`; `latticeSnapVoxel` drops its guard;
`ensurePart` takes `const Scene&`. Unblocks the reported case on its own, and is independent of
everything below.

**Stage 2 — the document lattice and the three snap layers.** ✅ **Built.** These landed together, for
the reason in the section above: generalising the lattice without the per-object flag widens the
pose-destruction hazard. `snapTransformToLattice` now quantizes in world space against
`documentSnapVoxel`; `snapStepWorld` and `snapsRotation` are the two gates, each with the same three
exemptions (setting off, component free, Alt held); `latticeSnapVoxel` is gone. Covered by
`ASSEMBLYTEST` 7d, which asserts all four properties and was checked to fail under the old rule
(0.5728 world units off grid inside an off-grid asset, 0.6 at the document root).

**Stage 3 — the `Place`/`Union` promotion.** ✅ **Built.** `setComponentOpInEditor` is now the single
funnel both op controls go through — the Assets panel row combo and the Place panel's radio row — and
it promotes the nearest placed row above when a row is set to `Subtract`/`Intersect` with nothing
above it that folds. `rowFolds` is the test, and it excludes `Grid` for the same reason `foldChildren`
does. Covered by `ASSEMBLYTEST` 7e (the promotion, the 256-cell fold, and one undo restoring both
ops — checked to fail under the old rule, folding to 512) and 7f (a stack that already folds is left
alone).

**Stage 4 — face snapping, and `Group into asset`.** The follow-up that makes composition pleasant
rather than merely possible. While dragging, if the moved component's content bounding box comes
within a few voxels of another's on an axis, snap the faces flush — the gesture "line this up with
that" literally means. And expose `wrapRootStack` as a **Group into asset** verb on a multi-selection,
so a boolean between two root-level `.data`s does not require knowing that ops only exist inside
assets.

---

## Hazards

1. **`ensurePart` on a Grid child.** `foldChildren` skips `Grid` components before it reaches the
   part lookup, so the order of those two checks must not be swapped — `componentContentBounds` on a
   grid volume is not something the fold should be walking. *(Stage 1: verified, the `Grid` check is
   upstream.)*

2. **`ensurePart` is no longer only called at adopt time.** It now runs from inside a fold, which
   runs from inside a frame. It allocates on first call per component, so a stack of many previously
   unregistered children pays a one-time content-bounds walk each. Bounded by the same budgets the
   fold already runs under, but worth watching on a large imported asset.

3. **A `Part` record for a component that is not a part.** After stage 1, a merged `.data` gets a
   `Part` with `procedural = false`. Anything keyed on `findPart` meaning "is a primitive" is now
   wrong. The Inspector's resize fields are the ones to check: they must gate on
   `part->procedural`, not on the record existing.

4. **Snapping in world space changes what undo replays.** Stage 2 makes the snapped value depend on
   the component's *ancestors'* transforms, not just its own. Moving an asset therefore changes what
   its children snap to. Idempotence still holds for a fixed ancestor chain, which is what the undo
   records need, but a reparent between record and replay is a case that did not exist before.
   *(Idempotence asserted by 7d; the reparent case is still open.)*

5. **The document lattice needs an origin that does not move.** Deriving it from content bounds would
   shift it every time anything is added — the same trap `latticeAtNode` avoids by deriving from the
   node rather than from the result chunk. It has to be the document root's frame, fixed. *(Built
   that way: world origin, world axes. The **size** has the analogous trap — deriving it from the
   finest voxel scale present means importing a finer asset re-phases the grid under everything
   already placed, so `snapVoxelOverride` pins it and derived results are skipped from the scan.)*

6. **`Free` and the fold.** A freely rotated row that carries an op still folds, and still costs the
   oriented-bounding-box walk. The flag must not be read as permission to skip the budget check.
   *(Held: `freeComponents` is consulted only by the two snap gates, never by `foldChildren`.)*

7. **`documentSnapVoxel` walks every component, from inside the transform funnel.** Once per
   transformed component per drag frame, plus a `resolveComponentVoxelSpace` each. Trivial beside the
   render, and the same walk `suggestedVoxelScale` already does — but it is now on a hot path, and a
   document with thousands of components is the case that would notice. Pinning the grid short-
   circuits it entirely, which is the escape hatch if it ever matters.

8. **Free placement does not survive a save.** `freeComponents` is editor-side, so reloading a
   document brings every deliberately-posed object back under the grid — and the first nudge
   straightens it. The pose itself is safe on disk (it is just a transform); what is lost is the
   exemption. This is the reason the optional `"snap": false` field is worth adding, and until it is,
   the hazard the whole per-object design exists to close is only closed within a session.
