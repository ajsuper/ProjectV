## Distinction:

Utils ought to handle all of our voxel related functionalities *(other than passing to OpenGL)*. This includes voxel related math functionalities, creating and converting data structures, and reading and writing voxel data to disk.

### Examples:
```markdown
Note: Below are mock functions; they don't exist in the engine.
```
```cpp
// ✅ This would belong in utils as it directly pertains to creating a voxel data structure.
projv::utils::createTree64()

// ❌ This would not utils as it does not pertain directly to handling voxel data.
projv::utils::createApplication();

// ✅ This would belong in utils as it pertains directly to a voxel related math functionality.
projv::utils::convertVoxelPositionToWorldPosition();
```

### Utils modules:
- **compose_io** -> Reading and writing compose folders (`compose.json` + `.data`): load, save, graft one into another.
- **attachments** -> Program data saved with components and folders, stored and written back but never interpreted by the engine.
- **scene_query** -> The component tree: find, list, transform, add, duplicate, delete, reparent.
- **editing** -> Per-component edit queues and `updateScene`, which applies them; chunk-to-grid growth.
- **voxel_management** -> Creating and converting voxel data structures: brick maps, tree64, geometry blobs.
- **voxel_math** -> Voxel-related maths.
- **material** -> Per-component palettes.
- **picking** -> Ray casts against the voxel scene.
- **animation** -> Motion sets for animated materials.

**No EnTT, and nothing from the runtime.** Utils is the voxel layer, and a tool with no game loop
must be able to use all of it. The `layering` ctest enforces this.

### More

For more information on this project, visit our [README.md](/README.md)