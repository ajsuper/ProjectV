# Sources

Below are the sources used in this project:

### Tree64 Data Structure
- [Nvidia's Sparse Voxel Octrees](https://research.nvidia.com/publication/2010-02_efficient-sparse-voxel-octrees-analysis-extensions-and-implementation) - Accesed on 13/6/2024 — Note: this paper describes a sparse voxel octree, which uses a branching factor of 2 per axis (2x2x2 = 8 children per node). Our tree64 uses a branching factor of 4 per axis (4x4x4 = 64 children per node), but the core structure and ideas are exactly the same, just with a different branching factor.

### Tree64 rendering
- [Nvidia's Sparse Voxel Octrees](https://research.nvidia.com/publication/2010-02_efficient-sparse-voxel-octrees-analysis-extensions-and-implementation) - Accesed on 13/6/2024 — Note: same as above; the traversal logic is identical in concept, differing only in branching factor (2 per axis giving 8 children per node for an octree, 4 per axis giving 64 children per node for our tree64).

### A-Trous Wavelet Transform denoiser
- [Holger Dammertz, Daniel Sewtz, Johannes Hanika, Hendrik P.A. Lensch](https://jo.dreggn.org/home/2010_atrous.pdf) - Accesed on 8/1/2024 

### More

For more information on this project, visit our [README.md](/README.md)
