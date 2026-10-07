# 35 — Surface Voxels (prototype)

Voxels as bounding boxes for surfaces. A solid voxel can carry one to three surface terms, and the
renderer clips their combination to the voxel's box. Occupancy still says where the surface is and
what the traversal visits; the terms say exactly where inside the box it runs.

Two styles from the same data:

- **Contours** — one plane per term. Faceted: the voxel grid still reads, with angled faces instead
  of cubes. The voxel aesthetic.
- **Quadrics** — one quadric per term. Smooth and faithful to the original mesh. The realistic look.

```bash
# Built-in analytic scene (sphere, cylinder, cone, ...):
cd build/examples/surface_voxels && ./surface_voxels

# Any mesh: voxelize with --surfaces, then view the folder.
cd build/examples/mesh_voxelizer
./mesh_voxelizer -f ../test_models/CesiumMilkTruck.glb -o ../surface_voxels/scenes/truck -r 256 --surfaces
cd ../surface_voxels && ./surface_voxels --scene scenes/truck
```

| Key | |
|---|---|
| 1 / 2 / 3 | voxels / contours / quadrics |
| 0 | baseline: the plain traversal with no surface lookup |
| 4 | sun shadows |
| 5 | tint hits that are box faces rather than surfaces (fallbacks, cuts) |
| 6 | paint leaks red (a surface hit from behind: a ray through a crack) |
| WASD / R F / mouse / scroll | fly, speed |

## How it works

**Voxelizer** (`--surfaces`, `examples/20-mesh-voxelizer`): voxelizes the shell as usual, then
flood-fills the grid from outside. Whatever the flood cannot reach is the interior, and is filled
solid. Every shell voxel is fitted from the triangles in and around it
(`projv::utils::fitSurfaceVoxel`):

1. one quadric (and, separately, one plane) when one surface fits;
2. otherwise the samples are split by normal and by offset, and two or three planes are combined —
   every way of nesting intersection and union is tried, so edges, corners, thin walls and steps all
   fit;
3. otherwise a best-effort approximation, rather than a protruding box.

Each fit is then oriented against the flood fill and made one-sided where the inside is known, so a
crack between neighbouring fits lands on solid material one voxel deeper (a dent, never a hole).
A fit that claims solid on a face open to the exterior is refitted with several terms. Open meshes
(no interior) stay two-sided sheets.

The result is `model.surfaces` beside `model.data`; the `.data` format is unchanged.

**Engine** (`include/utils/surface_quadrics.h`, `src/utils/surface_quadrics.cpp`): the sidecar is
quantised into two sets, contour and quadric, in one texture. Entries are found through the tree,
not a hash — the traversal already holds the leaf and the voxel's rank in it, exactly as for material
bytes. A plane is one 32-bit word; a curved voxel adds one word of tangent-plane curvature (or two of
full curvature where that is not accurate enough); multi-term voxels add a small block.

**Shader** (`include/pjv_surface.sc`): include it instead of `pjv_utils_DDA.sc`. It defines
`PJV_SURFACE_HOOK`, and the traversal asks it about every solid voxel it lands on: stop here (at the
surface, with its normal) or step on. A ray that passes through a box without touching its surface
costs one DDA step, not a new march.

## Measurements (256³, 840×1072, sun shadows on)

| | Armadillo | Milk Truck |
|---|---|---|
| Voxel data (tree64 + materials) | 0.29 MB | 1.01 MB |
| Contour set | 0.94 MB, 6.1 B per surface voxel | 1.27 MB, 7.0 B |
| Quadric set | 1.67 MB, 10.8 B | 1.63 MB, 9.0 B |
| GPU time: plain voxels | 6.5 ms | 4.3 ms |
| contours | 12.5 ms (1.9×) | 7.9 ms (1.8×) |
| quadrics | 14.1 ms (2.2×) | 8.3 ms (1.9×) |

## Measurement hooks

- `SURFACE_BENCH=<frames>` — interleaved timing of modes 0–3 (`SURFACE_BENCH_MODES=013` to pick).
- `SURFACE_CAPTURE=<dir>` — writes each mode's frame from the app itself (no desktop screenshot),
  plus leak views. `SURFACE_CAPTURE_LOOK="x,y,z,distance,yaw,pitch"` aims it;
  `SURFACE_CAPTURE_TINT=1` tints box-face hits.
- `SURFACE_PLANE_TOL`, `SURFACE_TANGENT_TOL` — quadric-set storage tolerances.

## Known gaps

- Meshes with holes (the Stanford bunny's base) get no interior and stay two-sided; closing them
  needs a robust inside test (generalized winding number).
- The interior flood fill runs over the whole grid at once; grids above 512³ skip it.
- A handful of thin trims still show box-face cuts; tune with key 5.
- Storage-LOD blobs are skipped, and a blob shared by several chunks takes the first chunk's
  surfaces.
