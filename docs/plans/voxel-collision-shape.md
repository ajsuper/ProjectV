# A voxel collision shape: bodies collide as their voxels

Status: **Phases 1-3 delivered** 2026-10-24 (see "Phase 1: delivered", "Phase 2: delivered", "Phase 3:
delivered"); Phase 4 (the default) not started. Follows from `voxel-storage-tiles.md` ("Related: physics
that reads voxels directly") now that tile tables are every blob's voxels.

> Measured on Teardown levels (Tuxedo Labs) converted from a local install with `mesh_voxelizer
> --td-*`. They are not in the repo and may not be redistributed, so the numbers here are a record,
> not something a checkout can rerun. Credit: `SOURCES.md`.

Goal: a voxel body's collision *is* its tile table, read in place, as Teardown's is its voxels. Nothing
is built when a body is made or edited, and nothing is approximated: no box decomposition, no box
budget, no content-box fallback. Breaking West Point Marina costs what the break costs, not a rebuild
of the world's collision.

## Why: what the marina measures (2026-10-24, restir-pt + tile tables, 45bcdaf)

Voxel Mayhem on the converted level, `MAYHEM_BENCH=blast` with a fixed `MAYHEM_BENCH_SHOT`, the
physics step timed by stage (temporary timers, removed):

| | ms |
|---|---|
| **Load**: the world's collision (describe: decoding and box-merging 4,140 chunks) | 1,980 |
| **Load**: Jolt creating the world's body from those boxes | ~100 |
| **The blast's step**, total | 157-178 |
| -- the strike (voxels near, shards laid out) | 29 |
| -- the island pass (local support) | 12 |
| -- describing the bodies (the world's 4.4, cached per chunk; 221 pieces ~3) | ~8 |
| -- **Jolt re-creating the world's body** (every change to it) | **99-102** (Phase 1: 1.4) |
| -- the simulation step | 7 |
| Then, with 215 more bodies: physics a frame | 6-10 |
| Physics in the first 0.5 s after the blast | 580-615 |

And what the boxes cost in correctness: even at a budget of 16,384 boxes a chunk (Mayhem's map
setting; the engine default is 1,024), **17 of the marina's chunks collide as their solid content
box** (53k-102k solid voxels each, which need more boxes than that). Debris lands on invisible walls
there.

So two costs, one cause: the world is one static body whose shape is a compound of every chunk's
boxes, and boxes have to be built.

(Phase 1 found the first cost was not the compound itself: Jolt's `StaticCompoundShape` constructor
asks every part for its mass properties and its sub-shape id bits, and a chunk's shape -- a compound
of its boxes -- works both out by walking every box, every time. Making the world's shape walked all
1.2M boxes in the world, twice. See "Phase 1: delivered".)

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
  (a 256^3 chunk: 24 bits, so a compound of up to 256 chunks), else the **brick's** (a 256^3 chunk:
  18 bits; a 1024^3 chunk: 24), with the voxel found again from the contact point.
- The static world stays one compound (Phase 1 kept it: a body per chunk brought seams between
  chunks), so its chunks share the bits: West Point Marina's 4,140 take 13, which leaves 19 -- a
  256^3 chunk's brick index, not its voxel's.

### The static world: a body per chunk (tried in Phase 1, not kept)

The world is one body because it was one shape. The idea was that each of its chunks be a static
body of its own:

- An edit changes one chunk's table, and its body's shape is swapped (`BodyInterface::SetShape`) --
  O(1), not a 100 ms re-creation. A chunk that empties is removed; one that appears is added.
- Bodies are made in chunk order, so every peer makes the same ones in the same order (NetId rules,
  `networking.md`).
- 4,140 static bodies in Jolt's broadphase is well within its design (static bodies sit in their own
  tree, never simulated); the counts in `PhysicsSettings` get the headroom (A-capacity).

This part does not need the custom shape: a body per chunk of *box* shapes already turns the 100 ms
into the cost of the changed chunks' boxes. It was Phase 1 for that reason, and it was built and
measured -- and not kept: Jolt removes internal edges (`mEnhancedInternalEdgeRemoval`) within a body
pair only, so a body sliding from one chunk's body onto the next catches on the edge between them.
The materials test's block on an ice floor of several chunks slid 4.98 m of the 5.70 m it should
(13% lost at seams); a replication test's prediction drifted past its millimetre. One body keeps
every seam inside one pair. What the per-chunk bodies did buy -- the 100 ms, and not waking the
level -- one body gets as well (Phase 1: delivered), and a voxel shape (Phase 2) answers for its own
mass, so the world's compound stays cheap to make.

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
| V2 | ~~One static body per chunk for the world~~ -- the world stays one body; its chunk shapes cache their mass properties and id bits (MassCachedShape), and an edit wakes only what its changed chunks touch | Measured both (Phase 1): the same speed, and a body per chunk catches at every chunk seam; the voxel shape fits one compound with brick-index sub-shape ids |
| V3 | Normals only from exposed faces | No seam catching on flat ground (a known box-compound flaw); the face masks make it cheap |
| V4 | Voxel vs voxel by surface corners plus edge-edge, as Teardown | Corners alone miss crossing edges; a full voxel-voxel SAT is too slow at thousands of voxels |
| V5 | Both representations behind `CollisionRepresentation` until the voxel shape is proven; then voxels the default | A6's seam exists for this; the box path is the reference to test against |
| V6 | Copy-on-write tables | The simulation must read a consistent snapshot; most edits touch chunks no shape holds |

## Phases

### Phase 1: the world's body cheap to change (box shapes)

Planned as a body per chunk; delivered as one body made cheap, after measuring both.

### Phase 1: delivered (2026-10-24)

Where the 100 ms went (temporary timers, removed): all of it in `StaticCompoundShapeSettings::Create`
for the world's 3,887 chunk parts. That constructor asks each part for its mass properties (to place
the compound's centre of mass) and checks its sub-shape id bits; a chunk's shape is a compound of its
boxes, and Jolt's compounds answer both by walking every box, each time they are asked. The world's
compound walked all ~1.2M boxes in the world twice: 98-122 ms for mass, then ~25 ms for the bits.

- **`MassCachedShape`** (`src/runtime/physics/mass_cached_shape.{h,cpp}`): a Jolt decorated shape of
  its own subtype (`User2`) around each chunk's compound of boxes, which works out the inner shape's
  mass properties and id bits once and passes everything else -- collide and cast against every
  subtype, rays, points, sub-shape ids -- to it unchanged. Jolt's shapes are `final`, so it is a
  decorator rather than a subclass. It takes no id bits, and `pathTo` passes through it. Collision is
  unchanged bit for bit: the golden recording replayed as it was with it alone, and all 353 tests
  passed. The world's shape: **98-122 ms -> 0.8 ms**; its body's re-creation **1.4 ms**.
- **An edit wakes only what its changed chunks touch.** Destroying the world's old body woke every
  body its bounds touch -- the whole level -- on every edit, which kept the marina's debris awake: a
  steady 2-3 ms a step more after each blast. `PhysicsWorld::destroyBody(id, wake)` wakes only the
  given boxes (recorded as `Op::DestroyWaking`, so a replay wakes the same); the runtime passes, for a
  static voxel body rebuilt because its voxels changed, each changed, added or removed chunk's box
  (its cube, or the pieces it had when it is gone). A rebuild for anything else (its values, its
  materials) wakes as before. Test: "an edit to a static body wakes what rests on the chunks it
  changed, and nothing else" (fails without it).
- **Golden:** regenerated (19,844 -> 19,900 bytes, 231 steps; LOG_VERSION 14). The session's wall is rebuilt with the
  targeted wake (two `DestroyWaking` ops for `Destroy`), a deliberate change to which bodies wake. It
  was not re-checked on Clang: no Clang on the machine it was made on.
- **Tried and not kept: a body per chunk** (the plan's V2; "The static world" above). Same speed
  (blast 402 ms over its first 0.5 s), but seams between chunks' bodies.

West Point Marina, `MAYHEM_BENCH=blast` (two runs each):

| | before | Phase 1 |
|---|---|---|
| Physics in the first 0.5 s after the blast | 580-624 ms | 409-412 ms |
| Worst frame | 157-178 ms | 65-66 ms |
| Physics a frame after 1 s | 4.9 ms | 3.0-3.1 ms |
| Pieces | 236 | 236 |

`MAYHEM_BENCH=cannon`: 27 ms in the first 0.5 s (was 26-31), 308 pieces as before. Load: the world's
body is made in ~1.4 ms instead of ~100; describing it (the box build, 1,980 ms) is Phase 2's.
Checks: unit tests (354), soak, net (5 of 5), Voxel Mayhem's, the sandbox's and the physics demos'
self-tests (two full runs, 79 of 79 checks; a third exited early without a result, as runs on main
do).

### Phase 2: the shape, for convex bodies against the world

- `VoxelShape`: bounds, ray, point, convex vs voxels, convex cast, materials, sub-shape IDs, recording.
  Static only (no mass properties yet); dynamic voxel bodies keep boxes.
- The world's chunks use it. The box budget and the content-box fallback stop applying to the world.
- Characters (`CharacterVirtual`), continuous collision, picking rays and the debris boxes all collide
  with it.
- Measured: load's collision 1,980 ms -> near zero; a convex resting on a flat voxel floor and sliding
  across it (no seam catches, which the box floor fails today); the marina's 17 content-box chunks
  collide as their voxels.

### Phase 2: delivered (2026-10-24)

Static voxel bodies collide as their tile tables when `CollisionParams::representation` is `Tiles`;
moving ones stay boxes (createBody refuses a moving body of tiles, logged). Boxes stay the default
(V5); Voxel Mayhem's maps use Tiles, and `PROJV_COLLISION=tiles` makes it what `PhysicsConfig` starts
with, to run anything on it.

- **The table is shared, copy-on-write** (V6): `GeometryBlob::tiles` a `shared_ptr`;
  `utils::shareBlobTiles` hands a shape the table as it is, and `editBlobTiles` (and
  `editBlobMaterials`) copy it first when anything else holds it. A blob copy (`forkBlob`) now
  shares its table too, until one is edited.
- **What collision reads** (`utils/tile_collision.h`, no Jolt; tested against a plain voxel grid in
  `test_tile_collision.cpp`): `tileBoxesIn` -- a full block one box, a block's full bricks merged
  greedily into boxes of one slot, a partial brick's voxels merged likewise within it; merged over
  the whole block or brick, so the boxes a body meets do not depend on where it is.
  `tileTableRaycast` -- a DDA through blocks, bricks, voxels, only where the level above holds
  something. `tileSurfaceNormal` -- the open face a point lies on.
- **`VoxelShape`** (`src/runtime/physics/voxel_shape.{h,cpp}`, Jolt `User1`): a convex against it
  collides with each box under its bounds as a Jolt box made on the stack (`CollisionDispatch`, so
  contacts, faces for manifolds, `CharacterVirtual`'s support and `mEnhancedInternalEdgeRemoval` are
  Jolt's own); a convex cast casts against the boxes under the sweep, nearest first, with early out;
  rays march the table; points, surface normals, supporting faces and materials (a palette slot's
  physics material) from the cell an id names. Registered both ways round against every convex
  subtype; compounds and decorators reach it through their own dispatch (debris box compounds,
  characters' capsules, continuous collision's casts, the runtime's queries).
- **Sub-shape ids: as fine as the bits allow.** Jolt keeps one contact manifold per pair of sub-shape
  ids, so boxes need ids of their own -- and a voxel index (a box by its first voxel) takes 24 bits at
  256^3, while a compound of n chunks takes ceil(log2 n) of the 32. The runtime gives each chunk's shape
  what its body's compound leaves (`voxelShape(..., idBits)`, cached apart by it):
  - up to 256 chunks of 256^3 (every test, the park): every box its own id, exact;
  - more (West Point Marina: 4,140 chunks leave 19 bits): ids name 2^k-voxel cells
    (`utils::tileCellShift`; the marina's are bricks), and a cell's boxes give one contact between them,
    the deepest. Measured first the other way -- one contact per brick everywhere -- and a character
    could not climb a 45-degree voxel staircase (tread and riser in one brick are one contact): so
    exact ids wherever they fit.
  Hits trace to their voxel as before: the path names the cell and its level, and the point finds
  the voxel in it (`resolve`).
- **Not built: normals from exposed faces only (V3).** Jolt's internal edge removal, run on the
  shape's boxes as it is on a compound's, is enough for what was measured: a frictionless box slid 12 m
  across a 4 x 4-chunk floor at 2 m/s keeps its speed (> 1.98 m/s) and does not bump (< 0.05 m/s up),
  on tiles and on boxes alike -- the box floor does not catch either, so the "fails today" this phase
  expected was not there. Kept for when something does catch.
- **Recording:** a tile shape is logged as its table's bytes, slot materials and id bits
  (`writeShape`, has-voxels value 2), and a replay builds the same shape. A second golden
  (`physics_golden_tiles.rec`, 24,355 bytes, 231 steps) records the session on tiles; the box golden
  is unchanged.

Measured, West Point Marina (`MAYHEM_BENCH=blast`, `MAYHEM_COLLISION=boxes` for the other column;
temporary timers, removed):

| | Boxes | Tiles |
|---|---|---|
| The world's collision at load (describe + create, 3,887 parts) | 2,052 ms | 26 ms |
| Chunks colliding as their content box | 17 | 0 |
| Physics in the first 0.5 s after the blast | 409 ms | 403 ms |
| Worst frame | 66 ms | 60 ms |
| Physics a frame after 1 s | 2.9 ms | 3.0 ms |
| Pieces | 236 | 225 |

(Fewer pieces: the break is the same, what the debris lands on is not.)

Tests (`test_physics_tiles.cpp`): a chunk over its box budget lets a ball down a shaft its content box
filled; the seam slide above; 20 balls at 200 m/s stopped by a wall one voxel thick; a shape given 12
id bits names bricks and still holds crates up; a moving body of tiles refused. The whole unit suite
passes on tiles (`PROJV_COLLISION=tiles`: 362 of 362), as do Voxel Mayhem's and the sandbox's
self-tests. The physics demos' self-test passes on tiles but for the Titans' crate check, which fails
in a full run on tiles and, run after demos 1 and 2, on boxes too (it is order-sensitive; alone it
passes on both).

Left for Phase 3 and after: moving bodies (mass properties, voxels against voxels); the coarse-id
case's lost contacts within a cell (a body of more than 256 chunks); exposed-face normals if a seam is
ever found; the tile golden on Clang (none on the machine it was made on).

### Phase 3: voxels against voxels

- Mass properties; dynamic bodies' chunks use the shape; voxel vs voxel (corners, edges, manifolds).
- Measured: the steady 6-10 ms a frame with 215 pieces on the marina, against boxes; the castle cannon
  and the park bench; contacts a pair.

### Phase 3: delivered (2026-10-24)

With `CollisionRepresentation::Tiles`, moving voxel bodies are tile shapes too, made for motion
(`shapeFromTiles(..., moving)`); static ones are as in Phase 2, so the static world and its numbers
are unchanged.

- **Weighed from the table, once**: each box's mass at its slot's density (as a compound of boxes'
  pieces: the material's, or otherDensity; 1 a voxel without materials), the centre of mass their
  centroid, the inertia theirs about it (parallel axis). Jolt hands shapes their queries in
  centre-of-mass space; every query adds the centre back (zero for a static shape). The materials
  test's two-material bar weighs and balances as it does as boxes.
- **What it collides by**: a moving shape merges its voxels across bricks within each 16^3 block
  once (`utils::tileBlockBoxes`), looked up by block; its bounds are its voxels' own box. (Its blocks'
  box -- a static shape's, harmless there -- made a 3-voxel piece reach a block's width round it, and
  the broadphase paired a pile of 400 pieces nearly all with all: 36,000 pairs a step, 4.6 ms against
  boxes' 1.07. With its own box: 1.13.) Its inner radius is its largest box's, as a compound's is.
- **Voxels against voxels** (`VoxelVoxelQueries`): the other's boxes where the two meet, fetched once
  (none, nothing to do); then the smaller shape's boxes there, each against those of the other its
  bounds reach, a Jolt box against a Jolt box; a pair of ids met twice (a coarse shape) keeps its
  deepest contact. Casts likewise, against the boxes under the whole sweep. Not Teardown's corners
  and edges: measured first, and at parity this was not needed.
- **Recording**: a tile shape logs whether it is for motion (LOG_VERSION 15); the tile golden now
  records moving bodies as tiles (52,776 bytes, 231 steps).

Measured (`PROJV_COLLISION` / `MAYHEM_COLLISION` for the other column):

| | Boxes | Tiles |
|---|---|---|
| A pile of 400 pieces on a voxel floor (`tiles bench`), ms a step | 1.04-1.07 | 1.13 |
| West Point Marina's barrage, ms a step (debris on) | 6.7-7.0 | 5.9-6.0 |
| The park's cannon: physics in the first 0.5 s / a frame after 1 s | 159 ms / 1.8 ms | 250 ms / 2.1 ms |
| The park's blast: the same | 228 ms / 1.9 ms | 321 ms / 2.0 ms |

The park's first half second after a break is the one place tiles cost clearly more (40-60%), and
the least repeatable number here (the two representations break the castle differently: 793 pieces
against 744, 347 against 1,474). Steady frames are within ~10%. Checks on tiles: the unit suite (369
of 369), Voxel Mayhem's and the sandbox's self-tests, the physics demos (78 of 79: the Titans' crate,
order-sensitive, fails after demos 1 and 2 on boxes too), soak, net (5 of 5).

Left: the park's post-break half second (where the cost goes: the shards' first contacts, or making
their shapes); Teardown's corners and edges, if a big flat piece face-down ever costs too much.

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
- **A big map** (`MAYHEM_MAP`, any converted level): load, the blast step, the steady frame, before and
  after each phase, in the table above.

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
- **Seams between bodies.** Jolt removes internal edges within a body pair only (Phase 1 measured a
  13% slide loss across chunk bodies), so the world stays one body. A dynamic body of several chunks
  is one compound for the same reason.

## Open questions

1. Edge vs edge: every exposed edge pair near each other, or only edges of voxels whose corners found
   nothing? (The second is cheaper; does it miss real contacts?)
2. ~~Should a static world chunk's body be per chunk, or per 4^3 chunks?~~ Neither: one body
   (Phase 1). Per-chunk bodies catch at the seams between them.
3. Is a dynamic body's shape per chunk (a compound for a grid body) or one shape over its chunks'
   tables? Per chunk matches the static world and the sub-shape bits.
