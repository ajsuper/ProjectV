#ifndef PROJV_CONSTRUCTED_RENDERER_H
#define PROJV_CONSTRUCTED_RENDERER_H


#include <string>
#include <utility>
#include <cstdint>
#include <unordered_map>
#include <string.h>
#include <vector>

#include "core/math.h"
#include "data_structures/texture.h"
#include "data_structures/framebuffer.h"
#include <bgfx/bgfx.h>

namespace projv {
    struct BGFXResources {
        bgfx::ShaderHandle defaultVertexShader;
        std::unordered_map<std::string, bgfx::UniformHandle> uniformHandles;
        std::unordered_map<std::string, std::vector<uint8_t>> uniformValues;
        std::unordered_map<uint32_t, bgfx::ShaderHandle> shaderHandles;
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
        // The animation motion table, and the time to sample it at. Engine-owned for the same reason
        // passTargetRes is: they are not a property of any one renderer's resources.json, and a
        // shader gets them by declaring them.
        //
        // Two vec4 per motion set, sixteen sets:
        //   [2i+0] = (kind, amplitude in voxels, spatial frequency, speed)
        //   [2i+1] = (direction.xyz, turbulence)
        // A row whose kind is 0 is unused, which is what every renderer that never sets one has.
        bgfx::UniformHandle pjvMotionSets;
        bgfx::UniformHandle pjvAnimTime;    // (seconds, 0, 0, 0)
    };

    // vec4 slots in the pjvMotionSets array: two per set, MAX_MOTION_SETS sets. Named here rather
    // than open-coded because the shader's own array declaration has to match it exactly.
    // Three vec4 per motion set, sixteen sets. The third row exists because ADVECTION needs parameters
// sway has no use for -- how fast a parcel burns out, how much its turbulence grows as it rises --
// and the first two rows had nothing spare. A sway set leaves it zero.
#define PROJV_MOTION_SET_VEC4S 48

    // Bound inputs a pass may describe through passInputRes. Sized to comfortably clear the widest
    // pass in the examples (the scene editor's shade, at five samplers); an input bound above this
    // slot still renders, it just cannot ask how big it is.
    #define PROJV_MAX_PASS_INPUTS 8

    // How many input textures a pass may bind before it starts destroying the scene. bgfx has sixteen
    // texture stages and addresses a binding by stage, so the later of two setTexture calls on one
    // stage simply replaces the earlier. The engine parks the scene (materialIDs, palette, looseList,
    // gridInfo, cellMap, tree64, headers) on stages 9..15, which leaves 0..8 -- nine -- for a pass's
    // own inputs, assigned in framebuffer-declaration order.
    //
    // Unlike PROJV_MAX_PASS_INPUTS above, exceeding this is not a soft loss of metadata: input ten
    // lands on stage 9 and the shader reads a uint scene texture through a float sampler. See the
    // binding order note in performRenderPasses -- the scene is bound FIRST so that a pass which
    // overruns loses the scene texture rather than its own input, and performRenderPasses warns once
    // per offending pass.
    #define PROJV_MAX_PASS_SAMPLERS 9

    // NOTE: this deliberately carries no size. A pass's size is its OUTPUT framebuffer's size, looked
    // up per frame through resolveTargetSize; there used to be windowWidth/windowHeight fields here,
    // set to 1/1 at construction and never read. Caching the resolved size here would just be a second
    // copy to keep in step with the textures, which is the bug class this whole cleanup is removing.
    struct BGFXDependencyGraph {
        std::vector<std::pair<bgfx::UniformHandle, uint32_t>> depdendencies; //UniformHandle = textureSampler, uint32_t = textureID;
        int targetFrameBufferID;
        bgfx::ProgramHandle shaderProgram;
        uint32_t renderPassID;
        uint32_t multiPassPassNumber;
        bgfx::UniformHandle multiPassPassNumberUniform;
    };

    struct ConstructedRenderer {
        BGFXResources resources;
        std::vector<BGFXDependencyGraph> dependencyGraph;
    };
}

#endif
