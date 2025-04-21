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
- fbo -> Creating FBO's (Frame Buffer Objects), adding textures to FBO's, adding FBO's to a render instance.
- render -> Rendering shaders to target FBO's, using FBO's as inputs to shaders.
- scene -> Passing a voxel scene to OpenGL to be rendered.
- shader -> Loading and compiling our shaders, adding our shaders to a render instance.
- uniforms -> Passing variables from the CPU to the GPU.
- user_input -> Handling the inputs from the user.
- window -> Handles creating our window, render instance, and callbacks.

### More

For more information on this project, visit our [README.md](/README.md)