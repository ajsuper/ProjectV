## TREE64 - v0.0

The 'Tree64' data structure is designed for effecient storage and ray marching in a heirarchical manner of a 3D voxel based scene. It uses a branching factor of 4 on each axis (4x4x4 = 64 children per node), as opposed to a traditional octree's branching factor of 2 (2x2x2 = 8 children per node).

### Important!

We first recommend you read the Nvidia Paper cited in our [SOURCES.md](/docs/SOURCES.md) To get an understanding of what our structure is based off of.

#### Terms:
- Child Node -> Referenced to by a Parent Node.
- Parent Node -> Points to a Child Node.
- Root Node -> The very first node in our tree64.
- Slice -> One node made up of a uint32_t.
- Relative Pointer -> A 31 bit number that points down the tree64 from this node to it's first child.
- Valid Mask -> A 64 bit mask (split across two uint32_t) that says which children of this node are valid (Either a leaf or another parent. Anything not empty).
- Leaf Flag -> A 1 bit mask that indicates whether this nodes children are leaves (1) or if they have children (0).

```
Important: 
The name of the nodes is relative for all except the root node. A child node that has children can be called a parent node.
There are no leaf nodes because the leaf's do not actually take up data, they are described by the valid mask and leaf flags.
```

### Splitting up of a grid into this structure.

Here is an image with an example drawing of how this sturcture works: [Example](/docs/images/IMG_9025.jpeg). In this illustration, we are in 2D so there are only 4 bits for the Valid Mask instead of 64 — a branching factor of 2 per axis in 2D gives 2x2 = 4 children per node, whereas our tree64 uses a branching factor of 4 per axis in 3D giving 4x4x4 = 64 children per node. Our relative pointer uses a decimal representation instead of a binary representation for simplicity. Also please visit my [Python Quadtree Ray Marcher](https://github.com/ajsuper/PythonQuadtreeRayMarcher) for a 2D example of the traversal logic and ray marching algorithm. Note that a quadtree uses a branching factor of 2 per axis (2x2 = 4 children per node in 2D) rather than the 4 per axis (4x4x4 = 64 children per node in 3D) of our tree64, so it is not a direct analogue — however the core logic and ideas are exactly the same, just with a different branching factor.

### File structure
- A single std::vector<uint32_t>
- Stored as raw binary

### Each node (3 x uint32_t)
- Structured as such [**32**Bits:Valid Mask 1][**32**Bits:Valid Mask 2][**31**Bits:Relative Pointer][**1**Bit:Leaf Flag]

### Storage LOD

Levels are serialized root-first, one contiguous run per level, with the leaf level last, and
child pointers are **relative** to the node that holds them. Together those two properties make
coarsening a tree64 a pure tail truncation:

1. Cut the array at the end of the level you want to keep. Every pointer in the retained prefix
   stays valid, because the prefix layout is byte-identical.
2. Set the leaf flag on the new last level. Its 64 mask bits, which meant "which children exist",
   now mean "which voxels are solid" — which is exactly what a leaf is.
3. Give each new leaf a material offset. There is one representative material byte per set bit,
   collapsed to a single byte when they all agree (the same uniform-leaf trick as a normal bake).

Do **not** re-run `addPointersTree64` on the result: it ORs pointer bits in rather than assigning,
so a second pass corrupts the already-pointered prefix.

One step drops one level, which is **4x coarser per axis** — never 2x. The resolution must stay an
exact power of 4, because both the shader and the CPU builder derive tree depth as
`log2(resolution)/2` with an implicit truncation. A 64³ tree goes 64 → 16 → 4; a resolution of 32
would be read as 2 levels and traversed as if it were 16³.

This is implemented by `downsampleTree64` (`src/utils/voxel_management.cpp`) and applied at GPU
upload time only — see `GeometryBlob::renderLOD` in
[scene_data_structure.md](/docs/data_structures/scene_data_structure.md). The CPU copy is always
full resolution.

### Block tables (GPU only)

The GPU march does not descend the tree from its root. At upload each tree gets a **block table**
(`utils/block_table.h`) appended to its range in the tree64 texture: one texel per 64³ region (a copy
of that region node's mask) and one per 16³ block (a copy of the block node's mask and the index of
its first child leaf, relative to the root). A ray reads those two from its cell's coordinate and then
the leaf — the tree's own node. Nothing below the 16³ level is copied, so the table costs a few percent
of the tree. It is derived, never stored: the `.data` format is unchanged.

Trees smaller than a region or a block (resolution 16 and 4) get a synthetic one-child entry. Every
tree the engine builds or downsamples keeps its leaves on its last level, which the table relies on
(`utils::buildBlockTable` counts any leaf above the 4³ level as a stray). See
[block-table-traversal.md](/docs/plans/block-table-traversal.md).

### More

For more information on this project, visit our [README.md](README.md)
