# A BVH over loose chunks: one spatial structure for rendering, picking and physics

Status: **proposal.** Nothing here is implemented.

## Why

Loose chunks are components that move on their own: the bodies the runtime's Scene bridge drives,
props, anything with its own transform. They have no acceleration structure on either side.

- **The shader** (`pjv_utils_DDA.sc`, the scene loop around line 3853) visits *every* loose chunk
  for *every* pixel: a header fetch, a quaternion-to-matrix, a slab test, and a full tree march
  whenever the box is hit. The only pruning is "skip a box that starts beyond the closest hit so
  far", and since the list is in creation order, that rarely helps.
- **The CPU picker** (`utils::pickVoxel`, `picking.cpp`) walks the same list the same way.
- **Physics** has no broadphase either. The sandbox tests every pair of bodies.

Measured in `examples/16-sandbox` (`SANDBOX_MEASURE=<bodies>`), on the dev machine's integrated
Radeon 840M/860M, 1600×900:

| | loose chunks | GPU / frame |
|---|---|---|
| arena as 5 big loose chunks, no bodies | 7 | 64.8 ms |
| arena as grids, no bodies | 0 | 34.4 ms |
| arena as grids + 150 resting balls | 150 | 209.6 ms |

The per-body cost is about **1.17 ms of GPU time per frame per body**. It is linear in the body
count, and it is the same whatever the arena is made of. At the sandbox's 300-body cap that is
over a third of a second per frame. Grids already have their own acceleration (a DDA over cells);
loose chunks need theirs.

## Decisions

### V1. A BVH over the world bounds of loose chunks

- **Leaves are loose chunks**, initially one per leaf. A leaf's bounds are the world AABB of the
  chunk's oriented cube (header position, rotation and scale), which the CPU already knows.
- **Interior nodes** are standard AABBs with two children.
- **The build is a binned SAH**, top down, on the CPU. A few thousand leaves build in well under a
  millisecond. A BVH rather than a uniform grid because loose chunks vary in size by orders of
  magnitude (a 3-unit ball, a 100-unit ship) and cluster (a pile of crates), and a grid serves
  neither.
- **Rebuild, then refit.** Rebuild whenever the set of loose chunks changes (spawn, delete, hide).
  When only positions changed, refit: recompute bounds bottom-up and keep the topology, which costs
  O(n). Rebuild anyway once the refitted tree's cost has drifted past a threshold, measured as SAH
  cost against the last rebuild. Start simple: rebuild every frame any loose header is dirty. Add
  refit when measurements say the rebuild shows up.

### V2. It lives in the voxel layer, as derived Scene data

`utils::LooseBVH`, built from a `Scene`, with no EnTT and no graphics, so tools and tests use it
like everything else in `utils`. Held on the Scene as a cache (`Scene::looseBVH`) with a dirty flag
set by anything that changes a loose chunk's header or the loose list: `rebakeSubtree`,
`deleteComponent`, the editor's hide/show, `instantiateComposeInto`. It is **derived**, so the
Scene's copy and move members either carry it or mark the destination dirty. Marking dirty is
simpler and cannot go stale.

Queries:
- `rayCandidates(ray, maxT, visit)`: nearest-first traversal, visiting leaves in roughly increasing
  entry distance. The visitor returns a new `maxT` to prune with.
- `overlaps(aabb, visit)`, and `selfOverlaps(visitPair)` for a physics broadphase.

### V3. On the GPU: packed into the loose-list texture, no new sampler

The shader library already binds texture slots 9–15, and renderers use the lower slots for their
own inputs. **The BVH does not take a new one.** The `looseList` texture (slot 11, `RGBA32U`)
grows a small header and a node region after the existing handle list:

```
node = 2 texels (8 × uint32):
  min.xyz, max.xyz as float bits   (6)
  left | leafFlag, right | count   (2)    -- interior: child node indices; leaf: first handle, count
```

`syncSceneTables` writes it whenever it rebuilds the loose list, which it already does on every
flush. The `looseCount` in `gridInfo` stays, so an empty scene or a shader built before this change
still works.

### V4. Shader traversal: one stack loop, one march call site

The scene loop's linear `for (i < looseCount)` becomes a stack traversal:
- a fixed-size stack (32 entries is enough for any tree this engine will build);
- the near child pushed last, so it is popped first;
- interior nodes pruned by `closestDistance`;
- leaves running the existing per-chunk body unchanged: local-frame slab test, then
  `castRayThroughTree64`.

**There is still exactly one `castRayThroughTree64` call in the loop.** This has been measured: a
second inlined scene-march call site (a second `raySceneIntersectFrom`) slowed every renderer mode
about 2×, even when it never ran, because inlining duplicates the whole march. The same reasoning
applies one level down. The traversal branches *around* the one call; it does not add a second. The transparency peel's
pass 0 (nearest and second-nearest entry) uses the same traversal without marching.

Below a threshold (say 8 loose chunks) the linear loop is kept. A tree over three boxes costs more
than it saves.

### V5. Grids stay as they are, for now

Grids are walked by their own DDA, and scenes have few of them. A later step can make the tree
top-level for *everything*, with grids as leaves that call `marchGrid`. That is the natural shape
(a top-level structure over components, each component's own tree64 or cell DDA underneath), but
it is not needed to fix the measured problem.

### V6. Tighter leaves come from content bounds

A leaf's box is its chunk's cube. In the sandbox that is an 8-unit cube around a 3-unit ball,
about 20× the volume, so most rays that enter it find nothing. Promotion candidate #4 (content
bounds: the tight box of a chunk's actual voxels) shrinks leaves to their content. It is a separate
change, it helps the GPU, the picker and physics at once, and it is already on the physics plan's
prerequisite list.

## Phases

### H1: `utils::LooseBVH` on the CPU
- Build (binned SAH), rebuild-on-dirty, ray and overlap queries.
- **Tests:** random scenes of rotated, scaled loose chunks. A ray query visits a superset of the
  chunks a brute-force slab test hits. Overlap queries match brute force exactly. An empty or
  one-leaf scene works. Deleting, hiding and moving chunks mark the tree dirty and the rebuilt tree
  matches.

### H2: `pickVoxel` uses it
- Visit leaves nearest-first, prune by the best hit.
- **Tests:** the existing picking behaviour (nearest wins, hidden chunks are skipped) on random
  scenes, compared against the brute-force path, which is kept behind a flag for exactly this.

### H3: The GPU traversal
- Pack nodes into `looseList`; traversal in `pjv_utils_DDA.sc`, with the linear loop kept below
  the threshold.
- **Correctness:** every renderer must produce the same image. Render a fixed scene with the old
  path and the new one and compare. The editor's self-tests and the renderer gallery are the broad
  check.
- **Exit:** `SANDBOX_MEASURE=150` close to the arena-only time. The target is a per-body cost
  under 0.05 ms, against 1.17 ms today.

### H4: A broadphase for the sandbox
- `selfOverlaps` replaces the all-pairs loop in the sandbox's `physics()`. This is the trial run
  for the physics plan's broadphase.

### H5 (separate change): content-bounds leaves
- Promote content bounds (#4); leaves use them.

## Risks

- **Register pressure on integrated GPUs.** A traversal stack in a shader that already carries a
  large tree64 march may spill. Measure on this machine first; it is the low end.
- **Every renderer includes `pjv_utils_DDA.sc`.** A traversal bug shows up everywhere at once.
  H3's image comparison exists for this.
- **Rebuild cost at scale.** Thousands of moving bodies rebuilt every frame is a CPU cost to
  measure, not assume. Refit (V1) is the answer if it shows.
