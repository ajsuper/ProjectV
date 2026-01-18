#ifndef CONSTRUCTED_RENDERER_H
#define CONSTRUCTED_RENDERER_H

#include <unordered_map>
#include <string.h>
#include <vector>

#include "core/math.h"
#include "data_structures/texture.h"
#include "data_structures/framebuffer.h"
#include "../../external/bgfx/include/bgfx/bgfx.h"

namespace projv {
    struct BGFXResources {
        bgfx::ShaderHandle defaultVertexShader;
        std::unordered_map<std::string, bgfx::UniformHandle> uniformHandles;
        std::unordered_map<std::string, std::vector<uint8_t>> uniformValues;
        std::unordered_map<uint, bgfx::ShaderHandle> shaderHandles;
        ConstructedTextures textures;
        ConstructedFramebuffers framebuffers;
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
}

#endif
