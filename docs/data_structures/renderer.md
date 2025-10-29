## renderer - v0.0

The 'renderer' is a folder containing resources.json, render.json, and shaders/ .

### File purpouse:
#### resources.json
This is where resources are specified. No rendering logic occurs here. This file includes defining uniforms, 
defining textures, defining frame buffers, and loading shaders.

##### Example:
```json
{
    "uniforms": [
        {"name": "cameraPos", "type": "vec4"}, 
        {"name": "windowRes", "type": "vec4"}
    ],
    "shaders": [
        {"shaderID": 1, "path": "./shaders/main.bin"},
        {"shaderID": 2, "path": "./shaders/post.bin"}
    ],
    "textures": [
        {"texID": 1, "name": "cpuTexture", "format": "RGBA8", "resX": 1920, "resY": 1279, "resizable": true, "origin": "CPUBuffer"},
        {"texID": 2, "name": "colorTexture", "format": "RGBA8", "resX": 1920, "resY": 1080, "resizable": true, "origin": "CreateNew"}
    ],
    "framebuffers": [
        {"fboID": 1, "textureIDs": 2}
    ]
}
```

##### Uniforms:
Uniforms are defined with just their name and the type. The supported types are vec4, mat3x3, and mat4x4 since our bgfx backend only supports these. They are set with setUniformToValue() in the gpu_interface module. They are automatically updated in all shaders whenever the renderer is executed.

##### Shaders:
Shaders are compiled fragment shaders. You have to compile GLSL fragment shaders with shadercDebug which is a bgfx tool that creates the .bin files. Shaders are written in GLSL, but they have some subtle differences in how they interact with the cpu. Please see PUTITHERE to understand how uniforms and textures are accessed. Each shader has a shaderID, and a shaderPath which will be loaded automatically.

##### Textures:
Textures have a textureID, name, format, resX, resY, resizable boolean, and origin. The origin, resizable, and resolutions have special relationships. See below:
```c++
if (resizable == true) {
    if (origin == "CPUBuffer") {
        // Will be resized based on the size of the texture passed to it.
    } else if (origin == "CreateNew") {
        // Will be resized based on window size.
    }
}
```

*note: it is recommended to generally set resizable to true. If you don't it prevents the engine from automatically resizing textures when needed.*

##### Framebuffers:
Framebuffers can be seen as a collection of CreateNew textures. They are used as render targets and as dependencies for render passes.

#### render.json
This is where rendering logic is specified. No resources are created. Render passes are defined with a shader and their dependencies. Their dependencies include the frame buffers they read from, textures passed from the cpu. Render passes are executed in order of which they occur.

##### Example:
```json
{
    "renderer": [
        {
            "shaderID": 1,
            "frameBufferInputIDs": [],
            "resourceTextures": [1],
            "frameBufferOutputID": 1
        },
        {
            "shaderID": 2,
            "frameBufferInputIDs": [1],
            "resourceTextures": [1],
            "frameBufferOutputID": -1
        }
    ]
}
```
##### ShaderID:
The ID of the shader to be executed for this render pass.

##### Frame buffer input IDs:
The ID's of framebuffers that the render pass depends on. Please see PUTITHERE to see how the textures are accessed in shaders.

##### Resource textures:
The ID's of the resource textures. These are CPUBuffer textures that can be manually set on the cpu. Please see PUTITHERE to see how the textures are accessed in shaders.

##### Frame buffer output ID:
The frame buffer ID that the shader will render to. Please see PUTITHERE to see how you render to specific textures.

### More
For more information on this project, visit our [README.md](README.md)
