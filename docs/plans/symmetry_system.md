# Symmetry System — Mirrored Editing on a Lattice (SceneEditor)

**Stage 1 implemented.** The live brush half — the frame, the mirror group, the sculpt and paint
hooks, the overlay and both self-tests — is built and documented in
[`docs/examples/SceneEditor/README.md`](../examples/SceneEditor/README.md) under *Symmetry*. Where
the two differ, the README is what the code does. Stages 2 onward below are design only.

The workflow this is built for, stated the way the user stated it:

> I was trying to create a pillar and it worked, but doing the same details 8x to cover it was a
> pain.

## Three planes do not solve that

The obvious design — three movable mirror planes — gives 8 **octants**. About a vertical axis that
is 4 copies, and the eighth only arrives by flipping top against bottom, which is not what a pillar
wants. So the obvious design misses the case that motivated it, and it misses it by exactly a factor
of two.

What a voxel grid actually permits is larger and more interesting than three planes:

> **A symmetry is a map from cells to cells, or it is a resample.** The maps that qualify are the
> ones whose linear part is a signed permutation of the axes. There are 48 of them, they are closed
> under composition, and every one is exact.

That set includes the **diagonal** mirrors — `x ↔ z` and its relatives — which are just as exact as
the axis mirrors and are what turn four copies into eight. The two axis planes plus the two diagonals
of a square are the dihedral group D4: identity, three 90° rotations and four mirrors, eight elements,
all of them signed permutations.

| Group | Copies about one axis | Exact? |
|---|---|---|
| One axis mirror | 2 | yes |
| Two axis mirrors | 4 | yes |
| Two axis mirrors + the diagonals (D4) | **8** | **yes** |
| Any of the above + the third axis mirror | ×2 | yes |
| Radial N | N | only N = 2, 4 — otherwise it rasterises |

So the diagonals are first-class toggles rather than something a user fakes by rotating a plane 45°,
and 8-fold detail on a pillar costs no accuracy at all. Free-N radial is a real feature and a later
stage; it is not pretended to be free.

## Two mechanisms, and they are not the same feature

**1. Symmetry as an edit transform.** The frame belongs to the thing being sculpted, and every dab
is replicated through the group as it is laid down. Sculpt, paint, fills. *(Stage 1.)*

**2. Symmetry as a part property.** A part carries a group, and the fold emits its cell set once per
element. Non-destructive: revise the detail and all eight follow at the next settle. *(Stage 4.)*

The pillar wants **the second one**. "Doing the details 8× was a pain" is really "and if I change one
I have to redo eight" — live brush symmetry only helps if all eight are sculpted in one gesture and
never revised. The first is what makes the editor feel like a sculpting app and is much the smaller
build, which is why it went first; the second is what closes the case, and the fold already does its
hard part.

The second is cheap here because `pullLeafIntoLattice` already inverse-transforms each lattice cell
centre into the part's voxel space. Hanging a group element on that transform yields a mirrored
instance with no new geometry and no push/pull hazard, and with mirrors the inverse stays a signed
permutation, so `cachedPartCells` stays exact.

**A mirror cannot be a component transform in this engine**, and that is why it has to live in the
pull or in the voxels. `ComponentRecord::localScale` is a `float` (`scene.h:130`), uniform only, so
there is no `(-1, 1, 1)` to express a flip with.

## Where the frame lives

**One frame per component, in that component's own voxel lattice.** Symmetry is a property of the
thing being sculpted rather than of the tool, so switching selection brings that component's own
mirrors back rather than dragging the last one along — and storing the origin in the component's own
coordinates keeps every mirror an exact integer map with no conversion anywhere in the inner loop.

Per component is also the honest scope for stage 1: a mirrored cell is written into the stroke's own
component and nowhere else, which is what "sculpt symmetry within one `.data`" asks for. Promoting
the frame to the **asset** — so a stroke on one part can land in its sibling — is stage 3, and needs
the stroke journal re-keyed before it would be correct. See below.

## The origin is stored doubled, and that is the interesting part

An axis mirror is `c' = origin[a] - c`, so the plane it describes sits at the float lattice
coordinate `(origin[a] + 1) / 2` — and the **parity** of `origin[a]` is the whole
centre-versus-boundary question that every voxel symmetry system has to answer and most answer
implicitly:

- **even** — the plane runs through a column of cell centres. That column is its own mirror image:
  unpaired, written once, no doubled seam.
- **odd** — the plane runs along a cell boundary. Nothing is fixed and every cell pairs off.

Storing twice the position as an integer expresses both exactly and keeps floats away from the one
place rounding would be visible. The panel shows `(origin + 1) / 2` and drags in half-voxel steps, so
`8.0` and `8.5` are both reachable and both mean something.

It also gives the diagonals a **parity condition**: the diagonal of the `(a, b)` plane maps cells to
cells only when `origin[a]` and `origin[b]` agree in parity, because otherwise it takes cell centres
onto cell corners. The toggle is greyed with a tooltip that says which two planes disagree and that
half a voxel fixes it — a statement about what the lattice allows, not a warning that the user has
made a mistake.

Centring falls out of the same arithmetic: `origin = contentMin + contentMax` puts each plane exactly
through the middle of the content, and the parity follows from the extent without anyone choosing it.
An even extent gets a boundary plane, an odd one a centred plane, and both are what a user means by
"the middle".

## The hook goes upstream of the journal

This is the part that is easy to get wrong in a way that looks almost right.

Symmetry is applied in **`stampSculptDab`**, on the way into `sculptStrokeOriginal`, and not in
`applyVoxelSculpt`. Everything downstream of that journal is derived from it: the undo and redo lists
`endSculptStroke` builds, and the solidity override that stops the brush chasing its own deposits.
Mirror at `applyVoxelSculpt` and the mirrored cells are written but never remembered — so undo
restores one copy of eight and the ray starts climbing back toward the camera at the seven mirrored
sites. Mirror where it is and undo, redo, the ray's lie, the near-plane double-write dedupe and
one-history-entry-per-gesture all come for free.

The group always contains the identity as its first element, so the unmirrored case is the same loop
running once and there is no second code path anywhere.

Two consequences worth stating:

- **Cells on a plane are their own image.** The journal's existing "already changed by this stroke"
  test handles them: the second group element finds the cell journalled and leaves it alone rather
  than queueing the same voxel twice.
- **The shape brushes replicate by cell; Smooth and Bump replicate by centre.** Mirroring an
  operator's *output* would reflect one site's result onto another site's shape; running the operator
  once per mirrored centre lets each copy read the geometry actually there. The brush sphere is
  symmetric, so a mirrored centre is an exactly mirrored dab. Copies whose boxes overlap near a plane
  run in sequence and read each other's writes — order-dependent by a cell or two, which is the same
  exception these brushes already make in reading current rather than stroke-start geometry.

Paint replicates the dab's centre and, for the two fills, the **seed**. Mirroring a fill's seed is
what a mirrored fill means: the traversal runs again from the mirrored voxel and finds whatever
region is there, which stays right even where the two regions are not congruent. The face normal goes
through the map's linear part. The material slot deliberately does *not* get re-read at the mirrored
seed — under the `Material` scope the slot is what the fill spreads through, and the copy should
spread through the same material as the original rather than through whatever sits at its own seed.

## Extrude mirrors faces, not dabs

Extrude has no dab to mirror: it fixes a face and a direction on the press and then reads one number.
So the press gathers a face at each mirror image of the seed, and the single depth the cursor supplies
drives all of them — **each along its own normal**. That is what makes the far copy a mirror rather
than a translation: on the other side of a plane "outward" points the other way, so a symmetric shape
grows at both ends instead of growing at one and being carved at the other.

The mirrored faces are **gathered, not derived**. Mapping the primary face's coordinates would assume
the geometry over there is the mirror of the geometry here, and where it is not, the extrusion pushes
cells belonging to no surface. Re-running `gatherFaceRegion` from the mirrored seed asks the geometry
instead of telling it — the same choice the mirrored fill makes about its seed. An image with no face
under it contributes nothing.

Faces are made **disjoint as they are gathered**, and the case that forces it is common rather than
exotic: a face straddling a mirror plane is its own image, so the mirrored gather returns the very
same region. Left alone it would be extruded twice — written twice into one layer record and counted
twice in the undo. Filtering at gather time rather than deduplicating at the write keeps every
structure downstream (the layer records, the redo walk) able to assume one cell belongs to one face.

The state this needed: `extrudeFace` / `extrudeFaceColors` / `extrudeNormal` became a list of
`ExtrudeFace { coords, colors, normal }`, with index 0 the clicked face and the one the cursor is
measured against. The layer records stay one per depth covering every face, because a layer is
reversed as a unit — the copies move together or the drag is not one gesture.

## The overlay is load-bearing

A frame whose origin has drifted off the geometry does not look like a broken symmetry. It looks like
a second object being built in empty space some distance away, often outside the view — and neither
the copy count in the panel nor the result on screen can say so, because the result is exactly what
the frame asked for. So the planes are drawn where they stand, dashed (a solid rectangle through a
model reads as geometry), colour-coded per axis with the diagonals in amber, sized to the content and
reaching a little past it.

Checkboxes and three numbers rather than a plane gizmo, deliberately. A gizmo would be a fourth thing
negotiating for the left button in a viewport where the tool, the picker and the transform handles
already do.

## Hazards

**1. Cost scales with the group order.** A radius-24 sphere dab is ~58k coordinates and every one
costs a tree64 descent; ×8 is ~460k per dab before interpolation. The existing radius and side caps
are the only budget, and the panel states the multiplier. A cap on `group × dab` is the natural next
guard if it bites.

**2. Smooth and Bump run N snapshot boxes per tick.** ~2.8 ms at eight copies against a 100 ms
budget, so affordable — but it scales with the group, not with the radius, which is not obvious from
the Strength slider that appears to govern the cost.

**3. Undo of a symmetric additive stroke inherits the empty-Grid-cell issue** already in the
SceneEditor README, eight times over.

**4. The panel edits the frame of the *selected* component, and a sculpt stroke goes to whatever the
ray hits.** The two are usually the same and every other readout in these panels already answers
against the selection, so this follows the existing convention rather than inventing a second one —
but a stroke that starts on an unselected object silently uses that object's own frame.

**5. Cross-component routing needs the journal re-keyed.** `sculptStrokeOriginal` is keyed by
`packVoxelKey(coord)` alone, which is only unambiguous while a stroke belongs to one component.
Stage 3 has to key it by `(component, coord)`, build the four undo lists per touched component, and
teach `makeSculptStrokeOverride` to answer for whichever component a chunk resolves to.

## Staging

1. **The frame and the mirrors on the brush, single component.** ✅ *Built.* Axis and diagonal
   toggles, integer origin with half-voxel display, centring, the plane overlay, replication in
   `stampSculptDab` and in the paint sample, Extrude's mirrored faces, and three self-tests.
2. **Flip and Rotate-90 as one-shot verbs** on a selection and on a part, reusing the same
   permutation maps. Small, and the escape hatch for when the live frame is not where you want it.
3. **Cross-component routing.** The frame promotes to the open asset; a mirrored cell goes to the
   component already solid there, else the part whose addressable box holds it, else the stroke's own
   component. Needs hazard 5 fixed first.
4. **Part-level symmetry in the fold** — the stage that actually fixes the pillar. A group on `Part`,
   N pulls, ghost outlines per instance, and a bake that emits the instances as real geometry.
5. **Free-N radial**, rasterising, labelled as such in the same voice `Snap 90`'s off-mode uses.
6. **Persistence.** A `symmetry` object on the compose entry, omitted when absent and defaulted off —
   the discipline `op: none` already follows. Reopening a half-finished symmetric model without its
   frame is a trap worth closing.

## Self-test

Two, because the group and the hook fail differently and a test of one passes with the other fully
broken.

`SYMMETRYTEST` — read-only arithmetic, no scene, so it runs under `EDITOR_SELFTEST`:

- the pillar case gives eight elements *and* eight distinct images, not four twice, with the free
  axis untouched;
- the group is closed under composition — the property the builder's fixed-point loop guarantees;
- every element applied four times is the identity;
- a frame centred on a box maps that box onto itself corner for corner, probed with a mixed-parity
  box including a one-voxel axis;
- an even origin fixes exactly one column and an odd one fixes none while pairing its two middle
  columns;
- a diagonal across mismatched parity is dropped rather than rounded into place.

`SYMSTROKETEST` — drives a real dab, so it runs under `EDITOR_SCULPTTEST`:

- one dab writes every copy, counted exactly (the dab is placed where all eight images are clear, so
  Add's skip-if-solid rule cannot make the number unpredictable);
- the written set is closed under the group — which a hook that mirrored only the dab's *centre*
  would pass on a symmetric brush and fail here;
- every copy reached the journal;
- and the one that matters: **undo restores the baseline exactly**, which is the check that fails
  loudly if anyone ever moves the hook downstream of `sculptStrokeOriginal`.

`ASSEMBLYTEST` 14 covers the mirrored extrude, on a box rather than the loaded scene precisely
because a box *is* symmetric, so the second face is guaranteed to exist and the counts are exact: two
8×8 faces, opposed normals, 128 voxels added by one layer, and undo back to the baseline. The opposed
normals are the assertion that catches the failure worth catching — driving both copies along the
*primary* normal extrudes one end and carves the other, which looks like a plausible asymmetric edit
rather than like a bug.

## An unrelated bug this fixed on the way

`Snap 90` rotation moved the object before it turned it, so an origin did not survive a rotation.

The rotate drag compensated its position against the **free** rotation from the cursor
(`pivotCompensatedPosition` at the gizmo) while `applyComponentTransform` went on to snap the
rotation to the nearest 90° afterwards. The compensation solves for the position that holds the pivot
under a given rotation, so being handed a rotation that never lands leaves the component displaced by
`(R_snapped − R_free) * pivot` — as large as the pivot's own distance from the origin. On screen: the
object slides steadily away while the ring is dragged, then jumps into orientation at each 90° step.

The rotation snap is now available on its own as `latticeSnappedRotation`, and the gizmo snaps
*before* compensating. `applyComponentTransform` still snaps on the way through, which is a no-op on
an already-snapped value, so the funnel keeps being the one place that governs this and the arrow
keys, the Inspector and the undo of all three are unaffected.

The position still rounds to the lattice *after* the compensation, so a rotation about a content
centre can move the origin by up to half a voxel: the compensated position is not generally on the
lattice, and landing on the lattice is the invariant that matters more.
