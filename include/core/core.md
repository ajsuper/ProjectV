## Distinction:

Core should handle the most fundamental logic for the engine. It should have no dependencies to any voxel related functionalities or graphics related functionalities. This should be completely independent and able to be used in any project.

### Examples:
```markdown
Note: Below are mock functions; they don't exist in the engine.
```
```cpp
// ✅ This would definetly belong in core, as it can be applicable to many different use cases other than voxel or graphial related.
projv::core::createEntity();

// ❌ This would not belong in core, as it most likely is not useful in a context other than graphical applications.
projv::core::initializeWindow();
```

### Core modules:
- ecs -> Contains the core functionalities for a ProjectV application, such as creating and managing an application, creating entities, managing components and systems.

### More

For more information on this project, visit our [README.md](/README.md)