## Distinction:

Utils ought to handle all of our voxel related functionalities *(other than passing to OpenGL)*. This includes voxel related math functionalities, creating and converting data structures, and reading and writing voxel data to disk.

### Examples:
```markdown
Note: Below are mock functions; they don't exist in the engine.
```
```cpp
// ✅ This would belong in utils as it directly pertains to creating a voxel data structure.
projv::utils::createOctree()

// ❌ This would not utils as it does not pertain directly to handling voxel data.
projv::utils::createApplication();

// ✅ This would belong in utils as it pertains directly to a voxel related math functionality.
projv::utils::convertVoxelPositionToWorldPosition();
```

### Utils modules:
- lod -> Handles changing the LOD of a voxel chunk.
- voxel_io -> Handles reading/writing of voxel data to and from disk.
- voxel_management -> Handles the voxel data, responsible for creating and converting voxel data structures.
- voxel_math -> Responsible for all of the voxel related math functionalities.

### More

For more information on this project, visit our [README.md](/README.md)