#ifndef CONSTRUCTED_RENDERER_H
#define CONSTRUCTED_RENDERER_H

#include <unordered_map>
#include <string.h>
#include <vector>


#include "../core/math.h"
#include "../../external/bgfx/include/bgfx/bgfx.h"

struct BGFXResources {
    bgfx::ShaderHandle defaultVertexShader;
    std::unordered_map<std::string, bgfx::UniformHandle> uniformHandles;
    std::unordered_map<std::string, std::vector<uint8_t>> uniformValues;
    std::unordered_map<uint, bgfx::ShaderHandle> shaderHandles;
    std::unordered_map<uint, bgfx::TextureHandle> textureHandles;
    std::unordered_map<uint, bgfx::UniformHandle> textureSamplerHandles;
    std::vector<int> texturesResizedWithWindow;
    std::vector<int> texturesResizedWithResourceTextures;
    std::unordered_map<uint, projv::core::ivec2> textureResolutions;
    std::unordered_map<uint, bgfx::TextureFormat::Enum> textureFormats;
    std::unordered_map<int, bgfx::FrameBufferHandle> frameBufferHandles;
    std::unordered_map<int, std::vector<uint>> frameBufferTextureMapping;
};

struct BGFXDependencyGraph {
    std::vector<std::pair<bgfx::UniformHandle, uint>> depdendencies; //UniformHandle = textureSampler, uint = textureID;
    int windowWidth;
    int windowHeight;
    int targetFrameBufferID;
    bgfx::ProgramHandle shaderProgram;
    uint renderPassID;
};

struct ConstructedRenderer {
    BGFXResources resources;
    std::vector<BGFXDependencyGraph> dependencyGraph;
};

#endif
