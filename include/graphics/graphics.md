## Distinction:

Graphics should handle all of the functionalities relating to OpenGL, FrameBuffers, Rendering, and similar concepts. Additionally it should handle user input as this closely relates to GLFW.

### Examples:
```markdown
Note: Below are mock functions; they don't exist in the engine.
```
```cpp
// ✅ This would belong in graphics as it pertains directly to rendering and not to our core functionalities nor the voxel functionalities.
projv::graphics::renderShaderToTargetBuffer();

// ❌ This would not belong in graphics as it pertains to handling voxel data.
projv::graphics::createVoxelScene();

// ✅ This would belong in graphics as it pertains to communicating to OpenGL and passing the scene.
projv::graphics::passSceneToOpenGL();
```

### Graphics modules:
- **render_instance** -> The window and bgfx: creating them, the active renderer, renderer specifications.
- **disk_io** -> Reading renderer descriptions (`render.json`, `resources.json`) and shaders from disk.
- **manage_resources** -> Turning a renderer specification into GPU objects: framebuffers, textures, programs, uniforms.
- **perform_renderer** -> Running a renderer's passes for one frame.
- **gpu_interface** -> Uploading a Scene to the GPU and keeping it current (`createTexturesForScene`, `flushSceneUpdates`).
- **input** (`graphics/input.h`) -> `projv::Input` and `installPlatform`: the per-frame platform system that polls the window, fills Input, and sends `CloseRequested` / `WindowResized`.
- **type_mapping**, **range_allocator** -> Internals of the above.

### More

For more information on this project, visit our [README.md](/README.md)