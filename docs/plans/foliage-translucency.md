# Foliage translucency in restir-pt

Proposed 2026-11-12. Status: **proposal**.

Leaves and grass blades are thin: sunlight striking one side shows through the other. restir-pt shades
every voxel as an opaque Lambert surface, so a canopy seen against the sun is a black silhouette, and
the light that comes down through a tree (green, soft) is missing from what is under it. The advanced
renderer has the direct half of this (`examples/40-advanced-renderer`, `compose.frag`
`foliageTransmission`, and the far-side sun ray in its `gbuffer.frag`). This plan brings the same into
restir-pt, and the indirect half too, since restir-pt traces paths.

## Decisions

**D1. Which voxels: the material's `transmission` byte.** The engine already carries one (word 3,
byte 2; `pjv_material.sc` decodes it, nothing acts on it; compose.json `"transmission"`). It is the
fraction of the light a voxel passes on through itself rather than reflects: 0 opaque (every palette
on disk -- the zero rule, nothing changes), around 0.3-0.5 a leaf. Not the advanced renderer's rule
(animated materials): a Teardown level has no animated materials, and translucency is a property of
the material, not of whether it sways.

**D2. The importer sets it.** Teardown's `foliage` substance (palette rows 9-24, and the tree voxscripts'
`CreateMaterial("foliage", ...)`) gets `transmission` 0.4. Ground grass (`unphysical` in ground.lua) is
solid ground in voxels, not blades: left opaque.

**D3. Direct light: the far side's sun ray.** For a transmissive voxel facing away from the sun, the
G-buffer's shadow ray (which today is not cast when `dot(n, toSun) <= 0`) is cast from the voxel's centre,
pushed past its sun-side face -- the advanced renderer's ray: it asks whether the sun reaches the
*other* side. The transmitted light is `transmission * albedo-tinted * sun * visibility * lobe(-L . V)`,
a forward lobe (Henyey-Greenstein, g about 0.5) so the glow is strongest looking towards the sun, in
`shadow_soft` beside the sun's reflected light. Cost: one ray per back-lit transmissive texel.

**D4. Thick foliage: transmissive voxels dim shadow rays instead of stopping them.** A Teardown tree's
canopy is a solid blob, many voxels deep. With D3 alone only the sun-side shell would ever glow (every
deeper voxel's far-side ray hits the next leaf). Instead a shadow ray through a transmissive voxel loses
a share of its light per voxel crossed (`transmission` as the per-voxel survival), using the traversal's
existing transparency query (`PJV_Q_TRANSPARENCY`, which already lets glass dim rather than block), so
light falls off through a canopy rather than stopping at its surface, and a tree's shadow is dappled
rather than solid. Open question 1: the cost of that query on a forest.

**D5. Indirect light: a diffuse transmission lobe in the paths.** `pjv_bsdf.sc` gains a third lobe:
Lambert on the back hemisphere, weight `transmission` (taken from the diffuse lobe, so energy is kept).
Paths then carry skylight and bounce light through leaves, and the reconnection shift is unchanged (the
lobe is rough). `pjv_bsdf.sc` is shared with pt-reference and the editor's path tracer: rule 1 holds
(transmission 0 is exactly today's BSDF), and pt-reference gets the same lobe, so it stays the
reference.

## Milestones

| # | What | Done when |
|---|---|---|
| F1 | D1, D2: the importer writes `transmission` for foliage | Lee Chemicals' trees carry it; nothing renders differently yet |
| F2 | D3: the far side's sun ray and the forward lobe | a tree against the sun glows at its edges; cost measured |
| F3 | D4: transmissive voxels dim shadow rays | the glow reaches into a canopy; its shadow is dappled; cost on a forest measured |
| F4 | D5: the transmission lobe, in restir-pt and pt-reference | the ground under a tree is lit green through it; restir-pt matches pt-reference on a test tree |

## Open questions

1. **D4's cost.** The transparency query reads a material at every leaf it crosses. On a forested level
   (Lee Chemicals' hills) this could be the most expensive change here; measure at F3 before deciding
   whether only the sun ray takes it (not the paths' rays).
2. **The value.** 0.4 for every foliage material, or by colour (lighter leaves pass more)? Judged by eye.
3. **The scene editor's material panel** could expose `transmission` (it already reads it). Not part
   of this plan.
