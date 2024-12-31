## VoxelTypeData - v0.0

The 'VoxelTypeData' structure is responsible for storing the data of each voxel such as color. Can be easily customizable, just don't forget to change the shaders too.

#### Terms:
- VoxelID -> The 1D Z-Order calculated from it's 3D position in the voxel grid.
- Data Slice -> The slice of data we want to store for each voxel.

### Sorting

Each voxel is sorted based off of it's VoxelID from least to greatest to allow for effecient searching in the shaders.

### File structure
- A single std::vector<uint32_t>
- Stored as raw binary

### Each Voxel's Default Configuration
- 1 Voxel = 2 Slices (64 Bits)
- VoxelID [**32**Bits:VoxelID integer]
- Data Slice for RGB color structured as such [**4**Bits:Empty][**10**Bits:Red][**10**Bits:Green][**10**Bits:Blue]

### More

For more information on this project, visit our [README.md](/docs/README.md)

Last edited on: 31-12-2024

