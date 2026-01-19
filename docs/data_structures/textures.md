## Interaction With Textures In Shaders

Shaders are programmed in GLSL, but have some subtle differences as to how textures are interacted with.

### Reading from frame buffer textures.
Assume this is your render pass:
```json
{
    "shaderID": 2,
    "frameBufferInputIDs": [1],
    "resourceTextures": [1],
    "frameBufferOutputID": -1
}
```

And this is your texture and frame buffer description:
```json
"textures": [
    {"texID": 1, "name": "cpuTexture", "format": "RGBA8", "resX": 1920, "resY": 1279, "resizable": true, "origin": "CPUBuffer"},
    {"texID": 2, "name": "colorTexture", "format": "RGBA8", "resX": 1920, "resY": 1080, "resizable": true, "origin": "CreateNew"},
    {"texID": 3, "name": "normalTexture", "format": "RGBA8", "resX": 1920, "resY": 1080, "resizable": true, "origin": "CreateNew"},

],
"framebuffers": [
    {"fboID": 1, "textureIDs": [3, 2]}
]
```

### Reading from textures.

Reading from textures requires knowledge of how textures are bound to the shaders. It is fairly simple.

Assume this render pass:
```json
{
    "shaderID": 1,
    "frameBufferInputIDs": [],
    "resourceTextures": [],
    "frameBufferOutputID": 1
}
```

And this is your texture and frame buffer description:
```json
"textures": [
    {"texID": 1, "name": "colorTexture", "format": "RGBA8", "resX": 1920, "resY": 1080, "resizable": true, "origin": "CreateNew"},
    {"texID": 2, "name": "normalTexture", "format": "RGBA8", "resX": 1920, "resY": 1080, "resizable": true, "origin": "CreateNew"},
    {"texID": 3, "name": "dirtPathTexture", "format": "RGBA8", "resX": 1024, "resY": 1024, "resizable": false, "origin": "CPUBuffer"}
],
"framebuffers": [
    {"fboID": 1, "textureIDs": [2, 1]}
]
```

Reading from these textures is simple. This is your GLSL shader:
```c
// Create our samplers.
SAMPLER2D(dirtPathTexture, 0); // SAMPLER2D will return all channels from 0-1, USAMPLER2D will return the value as an integer, but must be sampled with texelFetch.
SAMPLER2D(colorTexture, 1); // The first parameter sets the name of the texture for you to use in your shader later.
SAMPLER2D(normalTexture, 2); // The second parameter is the binding point, it is essentially the index of the texture.
// The binding point MUST start at 0 and increment by 1 for each texture.
// The textures MUST be in the order that they were in the frame buffer.

void main() {
    vec2 texCord = v_texcoord0.xy;
    vec4 dirtPathTexture = texture2D(dirtPathTexture, texCord);
}
```
Requirements:
- Resource textures are listed before all other textures, in the same order they are listed in in the render pass.
- The binding point MUST start at 0 and increment by 1 for each texture.
- The textures from frame buffers MUST be in the order that they were listed in in their frame buffer.

### Writing to the frame buffer targets.
Assume this render pass:
```json
{
    "shaderID": 1,
    "frameBufferInputIDs": [],
    "resourceTextures": [],
    "frameBufferOutputID": 1
}
```

And this is your texture and frame buffer description:
```json
"textures": [
    {"texID": 1, "name": "colorTexture", "format": "RGBA8", "resX": 1920, "resY": 1080, "resizable": true, "origin": "CreateNew"},
    {"texID": 2, "name": "normalTexture", "format": "RGBA8", "resX": 1920, "resY": 1080, "resizable": true, "origin": "CreateNew"},

],
"framebuffers": [
    {"fboID": 1, "textureIDs": [2, 1]}
]
```

Writing to the frame buffer in our render pass is simple:

```c
gl_FragData[0] = vec4(1.0, 0.0, 0.0, 1.0); // Index 0 is the textureID listed first in the frame buffer, so textureID 2.
gl_FragData[1] = vec4(0.0, 1.0, 0.0, 1.0); // Index 1 is the textureID listed second in the frame buffer, so textureID 1.

```
See how the index does NOT directly correspond to the texID of the texture. It is only dependent on the order they are listed in the target frame buffer.

*Note! If the frameBufferOutputID is -1, you are rendering to the window. The window is always rendered like this instead:*
```c
gl_FragColor = vec4(1.0, 1.0, 0.0, 1.0);
```

### More

For more information on this project, visit our [README.md](README.md)
