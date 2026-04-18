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

### More

For more information on this project, visit our [README.md](README.md)
