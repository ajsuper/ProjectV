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
        // vec4(width, height, 1/width, 1/height) of the target the CURRENT pass is writing into, set
        // by performRenderPasses before each submit. Engine-owned: it is not declared in
        // resources.json, and a shader gets it just by declaring `uniform vec4 passTargetRes;`.
        //
        // This is what a shader must use for anything measured in its own pixels -- sub-pixel jitter,
        // a pixel coordinate for a noise hash, filter tap offsets, a radius converted from world units
        // into pixels. It replaces reading a renderer-wide `windowRes`, which is correct only while
        // every pass shares one resolution and silently wrong the moment one of them does not.
        //
        // One uniform name reused across passes rather than one per pass: bgfx snapshots uniform
        // values at submit time, so each pass's draw call carries whatever was set before it.
        // (Contrast multiPassPassNumber, which is created per pass under a numbered name -- see the
        // note in the scene editor's denoise.frag about why that makes it unreadable from one shader.)
        bgfx::UniformHandle passTargetRes;
        // vec4(width, height, 1/width, 1/height) per BOUND INPUT of the current pass, indexed by the
        // sampler slot the engine bound it to -- so passInputRes[2] describes whatever texture is at
        // slot 2, matching the SAMPLER2D(..., 2) in the shader. Set alongside passTargetRes.
        //
        // This is what a pass needs once its inputs are not its own size. Two uses, both real:
        // POINT-SAMPLING a higher-resolution input (snap the UV to one source texel's centre, instead
        // of letting bilinear filtering average across a depth discontinuity and hand back a surface
        // that is not there), and UPSAMPLING a lower-resolution one (the tap offsets are the source's
        // texels, not the target's).
        bgfx::UniformHandle passInputRes;
    };

    // Bound inputs a pass may describe through passInputRes. Sized to comfortably clear the widest
    // pass in the examples (the scene editor's shade, at five samplers); an input bound above this
    // slot still renders, it just cannot ask how big it is.
    #define PROJV_MAX_PASS_INPUTS 8

    // NOTE: this deliberately carries no size. A pass's size is its OUTPUT framebuffer's size, looked
    // up per frame through resolveTargetSize; there used to be windowWidth/windowHeight fields here,
    // set to 1/1 at construction and never read. Caching the resolved size here would just be a second
    // copy to keep in step with the textures, which is the bug class this whole cleanup is removing.
    struct BGFXDependencyGraph {
        std::vector<std::pair<bgfx::UniformHandle, uint>> depdendencies; //UniformHandle = textureSampler, uint = textureID;
        int targetFrameBufferID;
        bgfx::ProgramHandle shaderProgram;
        uint renderPassID;
        uint multiPassPassNumber;
        bgfx::UniformHandle multiPassPassNumberUniform;
    };

    struct ConstructedRenderer {
        BGFXResources resources;
        std::vector<BGFXDependencyGraph> dependencyGraph;
    };
}

#endif
