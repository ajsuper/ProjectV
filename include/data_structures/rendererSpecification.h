#ifndef RENDERER_SPECIFICATION_H
#define RENDERER_SPECIFICATION_H

#include <string>
#include <vector>

#include "../../external/bgfx/include/bgfx/bgfx.h"

enum TextureOrigin { CreateNew, CPUBuffer };
enum UniformType {Vec4, Mat3x3, Mat4x4};

struct Uniform {
    uint uniformID;
    UniformType type;
    std::string name;
};

struct Shader {
    uint shaderID;
    std::string filePath;
    std::vector<char> shaderFileContents;
};

struct Texture {
    uint textureID;
    std::string name;
    bgfx::TextureFormat::Enum format;
    int resolutionX;
    int resolutionY;
    bool resizable;
    TextureOrigin origin;
};

struct FrameBuffer {
    uint frameBufferID;
    std::vector<uint> TextureIDs;
};

struct Resources {
    std::vector<Uniform> uniforms;
    std::vector<Shader> shaders;
    std::vector<Texture> textures;
    std::vector<FrameBuffer> FrameBuffers;
};

struct RenderPass {
    uint shaderID;
    std::vector<uint> frameBufferInputIDs;
    std::vector<uint> textureResourceIDs;
    int frameBufferOutputID;
};

struct DependencyGraph {
    std::vector<RenderPass> renderPasses;
};

struct RendererSpecification {
    Resources resources;
    DependencyGraph dependencyGraph;
};

#endif
