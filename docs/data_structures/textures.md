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

# Reading will soon be changed so the rest of this is not yet described.

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

*Note! If the frameBufferOutputID is -1, you are rendering to the window. The window is always rendered like this instead:*
```c
gl_FragColor = vec4(1.0, 1.0, 0.0, 1.0);
```

### More

For more information on this project, visit our [README.md](README.md)
