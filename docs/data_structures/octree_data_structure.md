## OCTREE - v0.0

The 'Octree' data structure is designed for effecient storage and ray marching in a heirarchical manner of a 3D voxel based scene.

### Important!

We first recommend you read the Nvidia Paper cited in our [SOURCES.md](/docs/SOURCES.md) To get an understanding of what our structure is based off of.

#### Terms:
- Child Node -> Referenced to by a Parent Node.
- Parent Node -> Points to a Child Node.
- Root Node -> The very first node in our octree.
- Slice -> One node made up of a uint32_t.
- Relative Pointer -> A 23 bit number that points down the octree from this node to it's first child.
- Valid Mask -> An 8 bit mask that says which children of this node are valid (Either a leaf or another parent. Anything not empty).
- Leaf Flag -> A 1 bit mask that indicates whether this nodes children are leaves (1) or if they have children (0).

```
Important: 
The name of the nodes is relative for all except the root node. A child node that has children can be called a parent node.
There are no leaf nodes because the leaf's do not actually take up data, they are described by the valid mask and leaf flags.
```

### Splitting up of a grid into this structure.

Here is an image with an example drawing of how this sturcture works: [Example](/docs/images/IMG_9025.jpeg). In this illustration, we are in 2D so there are only 4 bits for the Valid Mask instead of 8. Our relative pointer uses a decimal representation instead of a binary representation for simplicity. Also please visit my [Python Quadtree Ray Marcher](https://github.com/ajsuper/PythonQuadtreeRayMarcher) For a 2D example of our octree and ray marching algorithm.

### File structure
- A single std::vector<uint32_t>
- Stored as raw binary

### Each uint32_t (slice)
- Structured as such [**23**Bits:Relative Pointer][**8**Bits:Valid Mask][**1**Bit:Leaf Flag]

### More

For more information on this project, visit our [README.md](/docs/README.md)

Last edited on: 31-12-2024