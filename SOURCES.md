# Sources

Below are the sources used in this project:

### Tree64 Data Structure
- [Nvidia's Sparse Voxel Octrees](https://research.nvidia.com/publication/2010-02_efficient-sparse-voxel-octrees-analysis-extensions-and-implementation) - Accesed on 13/6/2024 — Note: this paper describes a sparse voxel octree, which uses a branching factor of 2 per axis (2x2x2 = 8 children per node). Our tree64 uses a branching factor of 4 per axis (4x4x4 = 64 children per node), but the core structure and ideas are exactly the same, just with a different branching factor.

### Tree64 rendering
- [Nvidia's Sparse Voxel Octrees](https://research.nvidia.com/publication/2010-02_efficient-sparse-voxel-octrees-analysis-extensions-and-implementation) - Accesed on 13/6/2024 — Note: same as above; the traversal logic is identical in concept, differing only in branching factor (2 per axis giving 8 children per node for an octree, 4 per axis giving 64 children per node for our tree64).

### A-Trous Wavelet Transform denoiser
- [Holger Dammertz, Daniel Sewtz, Johannes Hanika, Hendrik P.A. Lensch](https://jo.dreggn.org/home/2010_atrous.pdf) - Accesed on 8/1/2024 

### Teardown levels (testing only)
- [Teardown](https://teardowngame.com) by Tuxedo Labs. Its levels, converted from a local install by the mesh voxelizer's Teardown path (`examples/20-mesh-voxelizer/teardown/`), were the large real-world scenes the renderers, the tile tables and the physics (fracture, debris, impact loads) were measured and tuned on -- West Point Marina, Lee Chemicals, Hollowrock Island, Villa Gordon and others. None of their content is in this repository, and it may not be redistributed; the converter reads only what the person running it has installed.
- [TTFH's Teardown-Converter](https://github.com/TTFH/Teardown-Converter) - Accessed in October 2026 — Note: the reference for the level format's joints and transforms (`QuatToEuler`, `WriteJoint`, `Joint`); see `docs/plans/teardown-physics.md`.

### More

For more information on this project, visit our [README.md](/README.md)
