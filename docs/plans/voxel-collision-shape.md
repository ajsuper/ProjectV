# A voxel collision shape: bodies collide as their voxels

Status: **draft**, 2026-10-24. Nothing built. Follows from `voxel-storage-tiles.md` ("Related: physics
that reads voxels directly") now that tile tables are every blob's voxels.

Goal: a voxel body's collision *is* its tile table, read in place, as Teardown's is its voxels. Nothing
is built when a body is made or edited, and nothing is approximated: no box decomposition, no box
budget, no content-box fallback. Breaking West Point Marina costs what the break costs, not a rebuild
of the world's collision.

## Why: what the marina measures (2026-10-24, restir-pt + tile tables, 45bcdaf)

`MAYHEM_MAP=westpointmarina MAYHEM_BENCH=blast MAYHEM_BENCH_SHOT=50,60,-140,69.2,29.4,-44.8`, the
physics step timed by stage (temporary timers, removed):

| | ms |
|---|---|
| **Load**: the world's collision (describe: decoding and box-merging 4,140 chunks) | 1,980 |
| **Load**: Jolt creating the world's body from those boxes | ~100 |
| **The blast's step**, total | 157-178 |
| -- the strike (voxels near, shards laid out) | 29 |
| -- the island pass (local support) | 12 |
| -- describing the bodies (the world's 4.4, cached per chunk; 221 pieces ~3) | ~8 |
| -- **Jolt re-creating the world's body** (every change to it) | **99-102** |
| -- the simulation step | 7 |
| Then, with 215 more bodies: physics a frame | 6-10 |
| Physics in the first 0.5 s after the blast | 580-615 |

And what the boxes cost in correctness: even at a budget of 16,384 boxes a chunk (Mayhem's map
setting; the engine default is 1,024), **17 of the marina's chunks collide as their solid content
box** (53k-102k solid voxels each, which need more boxes than that). Debris lands on invisible walls
there.

So two costs, one cause: the world is one static body whose shape is a compound of every chunk's
boxes, and boxes have to be built.

## The design

### The shape

`VoxelShape`, a Jolt custom shape (`EShapeType::User1`, `EShapeSubType::User1`), one per chunk:

- It holds a `shared_ptr<const TileTable>` (the blob's voxels, not a copy: see "Sharing the table"),
  the chunk's `SlotMaterials` (palette slot -> physics material), and nothing else. Making one is
  O(1) beyond its bounds (the table's block box, from the header texel) and, for a dynamic body, its
  mass properties (below).
- Space: unit voxels, the chunk's corner at the origin, as `CollisionPiece`s are now; the body or the
  compound scales it (`ScaledShape` or the part's placement), as voxel shapes are placed today.
- The table's levels are the acceleration structure. A query's box in voxel space is clipped to the
  block box; regions and blocks with no occupied bit are skipped; a **full block** (a 16^3 tile) and a
  **full brick** (4^3) are tested as one box each; only partial bricks are tested voxel by voxel. A
  solid interior costs what a box costs.
- **Solid all the way through**, as boxes are (A6): a body that ends up inside is pushed out along
  the shortest way to an exposed face, never left inside a hollow surface.

### What it answers

Each is a function registered with Jolt (`CollisionDispatch::sRegisterCollideShape` /
`sRegisterCastShape` for User1 against every convex subtype and against User1; the shape's own
virtuals for rays, points and bounds):

1. **Ray** (`CastRay`): the CPU march of the table (`pjv_utils_DDA.sc`'s, in C++: region, block,
   brick, voxel, the box-clipped entry). The hit names its voxel and its face, so the normal is the
   face's (A6.3) and the hit traces to a voxel without a piece table.
2. **Point** (`CollidePoint`): `tileTableVoxel`.
3. **Convex vs voxels** (spheres, capsules, boxes, hulls; characters' capsules; debris boxes): the
   cells under the convex's box, as above. Each solid cell -- a tile, a full brick, a voxel -- is tested
   with Jolt's GJK/EPA against a box (`CollideConvexVsConvex`'s path, the box made on the stack).
   **Normals only point out of exposed faces**: a contact whose normal would point into a solid
   neighbour (the seam between two floor voxels) takes the normal of the exposed face instead, so a
   body sliding over a flat floor never catches a seam. Boxes have exactly this problem today; it is
   what Jolt's `InternalEdgeRemovingCollector` does for meshes, and what the face masks make cheap here.
4. **Convex cast vs voxels** (continuous collision's `LinearCast`, `CharacterVirtual`'s sweeps): the
   cells along the swept box, in the order the sweep reaches them, a GJK cast against each; the first
   hit ends it.
5. **Voxels vs voxels** (a piece against the world, a piece against a piece), Teardown's test:
   - the overlap of the two shapes' boxes, in the bigger one's space, at block level first (both
     occupied masks), so only blocks near each other are looked at;
   - the smaller one's **surface voxel corners** inside the overlap, each tested against the other's
     cells: a corner inside a solid cell is a contact, its depth and normal from the nearest exposed
     face of that cell;
   - **edge vs edge** for what corners miss (two rotated edges crossing, no corner inside): the
     surface voxels' exposed edges against the other's, only where both are near;
   - contacts grouped by the cell they hit and reduced to at most 4 a group, with the face polygon
     Jolt's contact manager builds manifolds from, so a flat piece resting on the floor is one
     manifold of 4 points, not thousands.
6. **Mass properties** (dynamic bodies): voxel count, centre of mass and inertia summed over the
   table -- a full tile or brick in closed form, partial bricks per voxel -- with each voxel's density
   from its material. Cached per content stamp, as shapes are now.
7. **Material** (`GetMaterial(SubShapeID)`): the voxel's slot's physics material.

### Sub-shape IDs

A contact or a hit has to lead back to a voxel (materials, damage, `ContactReports`). A Jolt
`SubShapeID` is 32 bits, shared with any compound above it:

- A chunk's shape uses the **voxel's index** when `3 * log2(resolution)` plus its compound's bits fit
  (a 256^3 chunk: 24 bits, so a compound of up to 256 chunks), else the **brick's** (a 1024^3 chunk:
  24 bits), with the voxel found again from the contact point.
- The static world stops being one compound (next section), so its chunks never share the bits.

### The static world: a body per chunk

The world is one body today because it was one shape. With voxel shapes, each of its chunks is a
static body of its own:

- An edit changes one chunk's table, and its body's shape is swapped (`BodyInterface::SetShape`) --
  O(1), not a 100 ms re-creation. A chunk that empties is removed; one that appears is added.
- Bodies are made in chunk order, so every peer makes the same ones in the same order (NetId rules,
  `networking.md`).
- 4,140 static bodies in Jolt's broadphase is well within its design (static bodies sit in their own
  tree, never simulated); the counts in `PhysicsSettings` get the headroom (A-capacity).

This part does not need the custom shape: a body per chunk of *box* shapes already turns the 100 ms
into the cost of the changed chunks' boxes. It is Phase 1 for that reason.

### Sharing the table

`applyTileEdits` changes a blob's table in place. A shape must see the voxels of the step it was made
for, not a later edit's half-applied ones, and the simulation thread must never read a table the main
thread is writing:

- The blob's tile table becomes **copy-on-write**: `GeometryBlob::tiles` a `shared_ptr<const
  TileTable>`, `editBlobTiles` copying it first when anything else holds it (a shape does). An edit
  of a chunk no shape holds costs nothing more than now; one that a shape holds costs a copy of that
  chunk's table (94k texels: ~0.4 MB, a fraction of a millisecond), and the shape is then swapped for
  one on the new table.

### Determinism

Everything in the step stays deterministic (`physics-jolt.md`): the shape's queries walk cells in a
fixed order (Z-order within a level, x-fastest over boxes), contacts are emitted and reduced in that
order, and nothing depends on thread timing (Jolt runs collision in parallel, but each pair's
collision is one call). The recording (`PhysicsSettings::record`) stores voxel shapes by their tables'
bytes (it stores box pieces now), so a replay rebuilds the same shapes.

The golden recording replays *box* collision today. While both representations exist, it keeps
replaying boxes, and a second golden records the voxel shape; when voxels become the default, the box
golden is retired (a deliberate change, regenerated and committed with it).

## Decisions

| | Decision | Why |
|---|---|---|
| V1 | The shape reads the blob's tile table, shared, not a copy or a derived structure | Nothing to build, nothing to keep in step; the table's masks are already the hierarchy collision wants |
| V2 | One static body per chunk for the world, in Phase 1, before the custom shape | Removes the 100 ms per change on its own, with box shapes; and the voxel shape needs it (sub-shape bits) |
| V3 | Normals only from exposed faces | No seam catching on flat ground (a known box-compound flaw); the face masks make it cheap |
| V4 | Voxel vs voxel by surface corners plus edge-edge, as Teardown | Corners alone miss crossing edges; a full voxel-voxel SAT is too slow at thousands of voxels |
| V5 | Both representations behind `CollisionRepresentation` until the voxel shape is proven; then voxels the default | A6's seam exists for this; the box path is the reference to test against |
| V6 | Copy-on-write tables | The simulation must read a consistent snapshot; most edits touch chunks no shape holds |

## Phases

### Phase 1: a static body per chunk (box shapes)

- `StaticCollider` components that split (a level's world) get one Jolt body per chunk, each its
  chunk's box shape; an edit re-describes and re-creates only the changed chunks' bodies.
- Measured: the blast step without the 100 ms (target: under 60 ms); load unchanged (the 2 s is the box
  build, Phase 2's).
- Golden: the recordings' scenes have one-chunk floors, so it should replay unchanged; if not, find
  out why before regenerating.

### Phase 2: the shape, for convex bodies against the world

- `VoxelShape`: bounds, ray, point, convex vs voxels, convex cast, materials, sub-shape IDs, recording.
  Static only (no mass properties yet); dynamic voxel bodies keep boxes.
- The world's chunks use it. The box budget and the content-box fallback stop applying to the world.
- Characters (`CharacterVirtual`), continuous collision, picking rays and the debris boxes all collide
  with it.
- Measured: load's collision 1,980 ms -> near zero; a convex resting on a flat voxel floor and sliding
  across it (no seam catches, which the box floor fails today); the marina's 17 content-box chunks
  collide as their voxels.

### Phase 3: voxels against voxels

- Mass properties; dynamic bodies' chunks use the shape; voxel vs voxel (corners, edges, manifolds).
- Measured: the steady 6-10 ms a frame with 215 pieces on the marina, against boxes; the castle cannon
  and the park bench; contacts a pair.

### Phase 4: the default

- Voxels the default representation, the box golden retired and the voxel golden kept, `pieceBudget`
  and `maxDecodedCells` only for the box representation (or the box path removed, if nothing needs it).

## Verification

- **Exact against the voxels:** for random tables (the tile-table tests' shapes), every ray and point
  query against a reference grid; every convex contact's point inside or on a solid voxel, its normal
  an exposed face's.
- **No seams:** a box and a sphere slid across a 64 x 64 voxel floor at 2 m/s: no vertical velocity
  above noise, no speed lost at seams (the same test on the box floor shows the seams).
- **Push-out:** a body placed overlapping solid voxels comes out by the shortest exposed face.
- **Tunnel (V3 of the physics plan):** 1,000 one-voxel balls at 200 m/s at a one-voxel wall; none pass.
- **Stacks and piles:** the physics demos' towers and piles stand as they do on boxes (the demos'
  self-test, every check).
- **Determinism:** a recorded session with voxel shapes replays bit for bit, on GCC and Clang
  (`--preset clang`).
- **The marina:** load, the blast step, the steady frame, before and after each phase, in the table
  above.

## Risks

- **Jolt's custom-shape surface is wide.** A shape type needs collide and cast functions against every
  subtype it meets, and Jolt's internals (manifold building, active edges) assume its own shapes. Kept
  within one file in `src/runtime/physics/` (the only place Jolt is included), tested pair by pair. A
  Jolt upgrade is a deliberate change already (AGENTS.md); this makes it a bigger one.
- **Voxel vs voxel cost.** A big flat piece face-down on the ground has thousands of surface corners
  touching. The block-level overlap, the reduction to 4 contacts a cell group, and sleeping bound it;
  Phase 3 measures it before anything depends on it. Teardown keeps debris small for this reason.
- **Fast small pieces.** Continuous collision needs a cast *of* a voxel shape (Phase 3); until then
  a dynamic voxel body casts as a conservative convex inside it (its largest inscribed box). Teardown
  substeps instead; that is the fallback.
- **Many contacts on flush faces** (a piece lying flat): the face polygon given to Jolt's manifold
  builder is what keeps it at 4 points; if Jolt prunes badly, the reduction is done before it sees them.
- **4,140 static bodies:** the broadphase's static tree is rebuilt as bodies are added; adding them in
  one batch (`AddBodiesPrepare` / `AddBodiesFinalize`) at load keeps it one build.

## Open questions

1. Edge vs edge: every exposed edge pair near each other, or only edges of voxels whose corners found
   nothing? (The second is cheaper; does it miss real contacts?)
2. Should a static world chunk's body be per chunk, or per 4^3 chunks (fewer bodies, an edit swaps a
   compound child)? Per chunk is simpler; measure the broadphase at 4,140 first.
3. Is a dynamic body's shape per chunk (a compound for a grid body) or one shape over its chunks'
   tables? Per chunk matches the static world and the sub-shape bits.
