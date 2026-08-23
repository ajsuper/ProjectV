# Animation

Status: **design agreed, nothing implemented.**

## The goal

This is the set of systems that takes ProjectV from static scenes — or scenes that only change
because a user edited them — to scenes that are *alive*. That framing is the point, and it is worth
stating before any mechanism, because it is what decides whether a given trade is worth making. An
engine that renders beautifully and never moves is a different product from one that does.

## The three kinds of motion

Named up front because they are genuinely different in what they demand, even though two of them
turn out to share one mechanism.

1. **Per-voxel detail.** Grass and leaf sway, water waves — real waves, the surface rising and
   falling with crests travelling across a choppy sea, not a shimmer. Periodic, looped, and
   procedurally generated.
2. **Large voxel movement within one `.data`.** Smoke rising and curling through a scene. The
   contents of the volume change.
3. **Whole-component movement.** Keyframed transform interpolation. The simplest of the three.

## The central decision: one warp, not three systems

Kinds 1 and 2 are the same mechanism: **a time-varying spatial warp, evaluated during traversal,
gated per material.**

Rising smoke is a vertical scroll of a warp field. An ocean is a Gerstner warp of a water surface.
Sway is a wind warp; leaf flutter is the same field at higher frequency. Flags, cloth, heat
shimmer, a breathing creature — all the same thing, with different parameters.

That is what earns it a place in the engine under the project's own test: one mechanical component,
wildly different content, no per-effect code. A "grass sway system" would be the Stone Crack brush
of animation. This is not that.

Kind 3 is separate and stays separate — it is rigid motion of whole parts, and it has nothing to
do with warping space.

### How the warp works

Do **not** displace the geometry. March the ray through undisplaced space with the sample position
warped:

    sample at  p' = p - d(p, t)      instead of  p

Geometry is never rebuilt, occupancy never changes, no memory is added, and silhouettes come out
right because the warp bends space rather than adding or removing anything.

**Why this is accurate: the fields are low frequency.** Waves and sway vary slowly in space, so the
warp gradient is small. The DDA already steps at cell granularity, which is exactly the step size
at which a small-gradient warp stays faithful. Cost is one field evaluation per step, paid only
inside flagged regions.

**Why not dilate the stored tree instead.** Bake-time dilation by the displacement bound D was the
obvious alternative and it is worse, specifically for the case that matters most. Grass is roughly
5% occupancy; dilating by 4 voxels can push it past 50% and destroy the sparsity that makes tree64
worth having. Warping costs nothing structural.

**Bound the displacement.** D = 3–4 voxels is the working figure. Everything below depends on the
bound existing; an unbounded warp has no cheap traversal story.

### The two things the warp must get right

- **Coarse-level skipping.** An empty node can still be entered by warped content, so the skip
  guarantee is lost inside flagged regions. Bounded by D, and at the upper levels D is sub-cell, so
  widening node bounds by D there is nearly free. Fine levels are being stepped through anyway.
- **Every ray, not just the primary.** Shadow and GI rays must apply the same warp, or grass casts
  a shadow that does not move and water reflects a surface that is not where it appears. Same
  function, more call sites — but missing one is a subtle, hard-to-attribute artefact rather than
  an obvious failure, so audit the entry points deliberately.

### What gates it

`Material::packedExtra` already reserves an 8-bit flags byte (`packExtraWord`'s third argument,
read by `materialFlags`). It is unused and zero-by-default, which means every palette already on
disk means "not animated" and nothing migrates. That is the gate.

A flag bit says *animated*; the warp field and its parameters are selected per material alongside
it.

### The warp field library — data, not scripts

A small parameterised set, not a scripting hook. Three cover the space:

| Field | Serves |
|---|---|
| **Gerstner** | Ocean and water surfaces. Crests sharpen, troughs flatten — the reason water reads as water. |
| **Directional sine, phase from position** | Grass sway, leaf flutter, flags, cloth. Per-voxel phase comes free from hashing world position, which is what stops a field from moving in unison. |
| **Curl-noise scroll** | Smoke, steam, heat shimmer. Divergence-free, so it curls rather than compressing. |

All three are cheap, analytic, and their parameters are numbers a material can carry. If a fourth
is ever needed it is a new entry, not a new system. Resisting a scripting hook here is deliberate:
this runs per DDA step in the hottest loop in the engine.

### Per-voxel scalar: the one format addition

tree64 stores a per-voxel **palette index**, not a per-voxel scalar. A participating medium wants
density as a *value*, and quantising it into palette slots would burn 255 entries and read badly.

The fix matches what is already there: **a second byte array parallel to `materialIDs`**, indexed
through the same leaf material offsets and honouring the same uniform-leaf flag. `DataBlock` gains
one array, `BlockEntry` gains one offset/length pair, `DataFile` goes to version 3, and absent
means "no scalar" so every existing file is still valid.

This is the only format change animation requires.

## Smoke

Smoke lives in the tree64, not in a dense 3D texture. The reasoning that settled it:

The expensive part of any participating medium is the ray from a scatter point to the light. A
dense grid makes that a brute-force march; the tree lets it skip empty air, and skip *to* the next
occupied region. Since the volume containing smoke is a small part of the scene, that is a large
win — and the structure is already a sparse compressed volume, so building a second, worse one
beside it would be perverse.

The pipeline is: **skip the medium during the g-buffer pass, then march it separately for the
smoke.** The path tracer already does the homogeneous case (`renderFogDensity`,
`renderFogAnisotropy`, Henyey-Greenstein, single-scattered from the sun); this is the heterogeneous
version of the same pass, sampling density from the per-voxel scalar array.

**Rebaking simulated smoke per frame is accepted.** Procedural smoke gets the warp and costs
nothing; genuinely simulated smoke — reacting to a moving object — rebuilds the tree each frame and
that is an accepted cost, not a problem to design around. It benefits later from the dense volume
window and kernel pass (items 7 and 8 of the promotion plan), but does not need them to work.

## Transform tracks — the rigid half

Kind 3, and the engine already owns almost all of it: parent/child hierarchy,
`localPosition/Rotation/Scale`, `setComponentTransform`, subtree rebake, `headerDirty` →
`updateDirtyHeaders`.

Parented components plus transform tracks give skeletal animation for nothing — doors, pistons,
fans, turrets, articulated characters, moving platforms. This is the right answer for rigid motion
of whole parts, and it does not compete with the warp; the two cover disjoint ground.

**The missing engine piece is not the curve, it is batched application.** N components getting new
transforms should be one rebake pass and one flush, not N subtree rebakes — and a child must not
rebake twice because its parent is animated too. A naive loop over `setComponentTransform` degrades
badly on a deep rig.

**Instancing is not the answer for fields.** Scatter-as-instances was considered for grass and
rejected on numbers: blades at ~2-voxel spacing is order 10⁴ components inside a single 256³ chunk
footprint — 10⁴ `ComponentRecord`s, 10⁴ GPU header rows, 10⁴ entries iterated by traversal and
picking, and 10⁴ transform writes and header rebakes per frame for per-blade phase. Two to three
orders of magnitude too expensive, and it does not apply to an ocean surface at all. Instancing
remains right for *props* — trees, rocks, a dozen bushes — which is a different problem.

## The track substrate

The one piece all of this shares is thin, and should stay thin: **keyframes, interpolation mode,
looping, sample at t.** A curve evaluator plus a binding from track to target. Each driver
(transform, material parameter, warp parameter) is tens of lines on top of it.

Do not build an "Animation" object that knows what a voxel is.

**Time is explicit, never a global.** `camera.h`'s `inline Camera cam` is the pattern not to
repeat. A renderer writing an image sequence and a voxelizer asked for frame 37 both need to sample
deterministically. Time is an argument, stored per-world.

The shader needs a time uniform, engine-owned in the same way `passTargetRes` and `passInputRes`
are — available to a shader that declares it, not declared in `resources.json`.

## Known costs and open questions

**Convergence — accepted, and already has a planned answer.** An animated scene never converges, so
the viewport's 64-frame average and the path tracer's TAA both break on exactly the content this
adds. This is accepted for now: the current renderer is a prototype. The intended fix is a
**per-face cache** that remembers recently visible voxels and voxels close to becoming visible,
which removes the issue at its root rather than patching the accumulator.

Worth noting that motion vectors are unusually easy here if they are ever wanted — the warp is
analytic, so where a surface was last frame is a closed-form question.

**Invertibility.** Warping the ray strictly wants `d⁻¹`. `p - d(p, t)` is a one-step approximation
with error on the order of `|∇d| · |d|`. Negligible for sway; worth measuring for a steep Gerstner
wave, where the gradient is deliberately sharp at the crests. If it shows, a second Newton step is
the cheap fix.

**Thin geometry.** Below roughly one voxel of width, grass is not voxel-representable and no
animation fixes that. A scope boundary, not a defect.

## Build order

1. **Time uniform + track evaluator.** Small, and everything else binds to it.
2. **Material flag + warp evaluation in the DDA**, one field only — directional sine. Prove the
   traversal change in isolation, on something whose correctness is easy to eyeball.
3. **Gerstner, on a water surface.** The best first real effect: a plane is the easiest geometry to
   debug, and wrong wave motion is unmistakable at a glance.
4. **Audit every ray entry point** for warp application — shadow and GI included.
5. **Sway on grass**, which is step 2's field with position-derived phase and nothing new.
6. **Per-voxel scalar array** (`.data` v3) and the heterogeneous medium pass.
7. **Curl-noise scroll** for procedural smoke.
8. **Transform tracks + batched rebake.** Independent of everything above; can be done at any
   point.

## Record of the design debate

Kept so it is not re-litigated. Three positions were argued and overturned:

- **"Make grass instanced components with transform tracks."** Rejected on density — see above. The
  count is off by orders of magnitude and it does not generalise to water at all.
- **"Displacement breaks the tree64."** Overstated. Warping the ray rather than the geometry, with
  a bounded low-frequency field, makes it a payable per-step cost confined to flagged regions
  rather than a structural impossibility.
- **"Smoke should be a dense 3D texture, not voxels."** Wrong. tree64 is already a sparse
  compressed volume, and it accelerates the scatter-point-to-light ray that dominates the cost —
  which a dense grid cannot.

The surviving synthesis — one warp serving waves, sway, flutter and smoke alike — is stronger than
any of the per-effect proposals it replaced, and is the reason this is one document rather than
three.
