## Shaders

This document explains how to write shaders for ProjectV.

### Textures
For handling textures please see [./textures.md].

### Uniforms
For handling uniforms please see [./uniforms.md]

At the very top of every shader, you should have the following:

```c
$input v_color0 
$input v_texcoord0

#include <bgfx_shader.sh>

// List your uniforms

// And then your shaders

void main() {
    // ...
```

### Compiling shaders:
The biggest difference from ProjectV GLSL and normal GLSL is that the shaders have to be compiled. This is because of the BGFX rendering backends way of making cross platform rendering possible.

#### How to do it:
You simply run the tool ``shadercDebug`` (``shadercRelease`` for release builds) located in ``/ProjectV/build/tools/shadercDebug``
#### Example:
```
./shadercDebug -f shader.frag -o shader.bin --type f --platform linux --profile spirv -i /ProjectV/external/bgfx/src

-f shader.frag The shader to compile
-o shader.bin The directory and name where the compiled shader should be
--type f The type of shader is a fragment shader
--platform linux The platform is linux
--profile spirv We are compiling for Vulkan backend (default on linux)
-i /ProjetV/external/bgfx/src include directory for bgfx's default shader stuff that allows the compiler to work.
```

### Shader that uses ProjectV data structures.
ProjectV has a shader that is provided that you use to actually render the ProjectV scene. It has 3 textures already defined:
- octreeData, at binding point 13.
- voxelTypeData, at binding point 14.
- headerData, at binding point 15.

They are all sampled with USAMPLER2D, so they will return integers when sampling with texelFetch.

