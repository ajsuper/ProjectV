// ReSTIR GI (examples/shared/restirRenderer/): two bounces of light traced for a sixteenth of the
// pixels and shared between pixels and frames by reservoir resampling, denoised, over a half-resolution
// G-buffer rebuilt to the window, with a soft sun shadow, SSAO, distance fog and a post (exposure, grade,
// vignette, depth of field). The passes and the look: restirRenderer/README.md.
//
//     projv_add_example(name ... RENDERER ../shared/restirRenderer INCLUDE_DIRS ../shared)
//
//     restir_renderer::install(world, renderInstance, here);                         // at startup
//     restir_renderer::draw(world, renderInstance, gpu, scene, eye, forward, sun);   // each frame
//
// Its settings are the View in world.ctx(); it keeps last frame's camera and sun there too, so a caller
// passes only where it is looking from and where the sun is.
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>

#include "core/math.h"
#include "core/world.h"
#include "graphics/disk_io.h"
#include "graphics/gpu_interface.h"
#include "graphics/manage_resources.h"
#include "graphics/perform_renderer.h"
#include "graphics/render_instance.h"

namespace restir_renderer {
    using projv::core::vec3;
    using projv::core::vec4;

    struct View {
        int   giStride = 4;            // GI for one window pixel in stride x stride: 4 (a sixteenth) or 2
        int   debugView = 0;           // 0 the image, 1 indirect only, 2 direct only, 3 ambient occlusion
        bool  reuse = true;            // ReSTIR's temporal and spatial reuse; off: each GI pixel's one sample
        bool  reconstructBypass = false;
        bool  softShadow = true;
        float sunHalfAngle = 0.02f;    // radians, ~4x the real sun's: penumbrae a little soft
        bool  fog = true;
        float fogLength = 300.0f;      // world units over which 63% of a surface's colour is lost
        // The post, in linear light before the tone curve; each the identity when off.
        bool  post = true, vignette = true, grade = true, exposure = true, dof = true;
        float vignetteAmount = 0.15f;
        float dofPixels = 1.5f;        // the far distance's blur radius at 1080 lines
        // The finest voxel's edge: rays leave a surface a twentieth of it out, the SSAO reaches six of
        // them, and the same-surface tests scale from it. 0: measured from the scene at the first upload.
        float finestVoxel = 0.0f;

        // History, kept here so a caller need not.
        vec3  prevPosition{0.0f}, prevDirection{0.0f, 0.0f, 1.0f}, prevSun{0.0f};
        bool  hasPrevious = false;
        int   frame = 0;
        std::chrono::steady_clock::time_point lastFrame = std::chrono::steady_clock::now();
    };

    static constexpr float RENDER_SCALE = 0.5f;   // its G-buffer, each way: resources.json's 0.5 targets

    // Loads the renderer staged beside the binary (`here`/`folder`) as the active renderer, with its GI
    // targets sized for the View's stride. Emplaces a View if there is none.
    inline void install(projv::World& world, projv::graphics::RenderInstance& renderInstance,
                        const std::filesystem::path& here, const std::string& folder = "restirRenderer") {
        View& view = world.ctx().contains<View>() ? world.ctx().get<View>() : world.ctx().emplace<View>();
        projv::RendererSpecification spec =
            projv::graphics::loadRendererSpecification((here / folder).string() + "/");
        // The GI targets are declared at a quarter each way; the stride says how far below the window
        // they really are.
        for (projv::Texture& texture : spec.resources.textures)
            if (texture.scale == 0.25f) texture.scale = 1.0f / float(view.giStride);
        renderInstance.addRendererSpecification(1, spec);
        bgfx::ShaderHandle vertexShader = projv::graphics::loadShader((here / folder / "shaders/vs_quad.bin").string());
        renderInstance.setActiveRenderer(
            projv::graphics::constructRendererSpecification(renderInstance.getRendererSpecification(1), vertexShader));
    }

    // Sets this frame's uniforms from the View, the camera and the sun.
    inline void upload(projv::World& world, std::shared_ptr<projv::ConstructedRenderer> renderer,
                       projv::core::vec2 windowResolution, vec3 position, vec3 direction, vec3 sunDirection) {
        using projv::graphics::setUniformToValue;
        View& view = world.ctx().get<View>();
        if (view.finestVoxel <= 0.0f) {
            if (const projv::Scene* scene = world.ctx().find<projv::Scene>())
                for (const projv::Chunk& chunk : scene->chunks)
                    if (chunk.alive && chunk.header.voxelScale > 0.0f)
                        view.finestVoxel = view.finestVoxel > 0.0f ? std::min(view.finestVoxel, chunk.header.voxelScale)
                                                                   : chunk.header.voxelScale;
        }
        const float finest = view.finestVoxel > 0.0f ? view.finestVoxel : 1.0f;
        const bool first = !view.hasPrevious;
        if (first) {
            view.prevPosition = position;
            view.prevDirection = direction;
            view.prevSun = sunDirection;
            view.hasPrevious = true;
        }
        const bool sunMoved = glm::length(sunDirection - view.prevSun) > 1e-6f;
        const bool moved = sunMoved || glm::length(position - view.prevPosition) > 1e-5f ||
                           glm::length(direction - view.prevDirection) > 1e-6f;

        // Which render texel of its block each GI pixel samples this frame (the block: the GI stride
        // against the render resolution, 2 or 1): every one in turn, diagonals first.
        const int giStride = std::max(1, int(float(view.giStride) * RENDER_SCALE));
        static const int order4[4][2] = {{0, 0}, {1, 1}, {1, 0}, {0, 1}};
        const int* offset = order4[giStride == 2 ? view.frame % 4 : 0];
        // Each render texel is traced through the centre of another of the 2x2 window pixels it covers
        // each frame: a still view repeats the same four, and the resolve settles on them.
        static const int cellOrder[4] = {0, 3, 1, 2};
        const int cell = cellOrder[view.frame % 4];
        vec4 jitterParams{(float(cell % 2) + 0.5f) * 0.5f - 0.5f, (float(cell / 2) + 0.5f) * 0.5f - 0.5f, 0.0f, 0.0f};
        vec4 renderParams{0.0f, 0.0f, view.reconstructBypass ? 1.0f : 0.0f, 0.0f};
        // SSAO six of the finest voxels round, darkening the indirect light by up to half; the sun's
        // penumbra per unit of blocker distance (the disc's full width); the fog's density.
        vec4 look{6.0f * finest, 0.5f, view.softShadow ? 2.0f * std::tan(view.sunHalfAngle) : 0.0f,
                  view.fog && view.fogLength > 0.0f ? 1.0f / view.fogLength : 0.0f};

        vec4 cameraPos{position, 1.0f}, cameraDir{direction, 0.0f};
        vec4 prevCameraPos{view.prevPosition, 1.0f}, prevCameraDir{view.prevDirection, 0.0f};
        vec4 sunDir{sunDirection, 0.0f};
        // z: the sun moved -- the light changed, so every history (the reservoirs' cached light
        // included) starts over.
        vec4 frameCount{float(view.frame), moved ? 1.0f : 0.0f, sunMoved ? 1.0f : 0.0f, 0.0f};
        vec4 reuse{view.reuse ? 1.0f : 0.0f, view.reuse ? 1.0f : 0.0f, 1.0f, 4.0f};   // temporal, spatial, visibility, M cap
        vec4 denoise{1.0f, 1.0f, float(view.debugView), 12.0f};                    // a-trous, GI accumulation, view, its cap
        vec4 jitter{float(offset[0]), float(offset[1]), float(giStride), first ? 1.0f : 0.0f};
        // w: one render texel's width per unit of distance, for the "is this the same surface?" tests.
        const float renderHeight = std::max(windowResolution.y * RENDER_SCALE, 1.0f);
        const float pixelWidth = 2.0f * std::tan(30.0f * 3.14159265f / 180.0f) / renderHeight;
        // The image's history: none when the sun moved (the light is new); short while the camera
        // moves, so changing light is not dragged along; 64 frames on a still view.
        const float imageHistory = sunMoved ? 1.0f : moved ? 16.0f : 64.0f;
        vec4 settings{6.0f, finest * 0.05f, imageHistory, pixelWidth};
        setUniformToValue(renderer, "cameraPos", cameraPos);
        setUniformToValue(renderer, "cameraDir", cameraDir);
        setUniformToValue(renderer, "prevCameraPos", prevCameraPos);
        setUniformToValue(renderer, "prevCameraDir", prevCameraDir);
        setUniformToValue(renderer, "sunDir", sunDir);
        setUniformToValue(renderer, "frameCount", frameCount);
        setUniformToValue(renderer, "restirReuse", reuse);
        setUniformToValue(renderer, "restirDenoise", denoise);
        setUniformToValue(renderer, "restirJitter", jitter);
        setUniformToValue(renderer, "restirSettings", settings);
        setUniformToValue(renderer, "restirLook", look);
        // The post's switches, and the frame's length for the exposure's adaptation.
        const auto now = std::chrono::steady_clock::now();
        const float seconds = std::chrono::duration<float>(now - view.lastFrame).count();
        view.lastFrame = now;
        vec4 postParams{view.post && view.vignette ? view.vignetteAmount : 0.0f, view.post && view.grade ? 1.0f : 0.0f,
                        view.post && view.exposure ? 1.0f : 0.0f, seconds};
        setUniformToValue(renderer, "restirPost", postParams);
        vec4 dof{view.post && view.dof ? view.dofPixels * windowResolution.y / 1080.0f : 0.0f, 0.0f, 0.0f, 0.0f};
        setUniformToValue(renderer, "restirDof", dof);
        setUniformToValue(renderer, "jitterParams", jitterParams);
        setUniformToValue(renderer, "renderParams", renderParams);

        view.prevPosition = position;
        view.prevDirection = direction;
        view.prevSun = sunDirection;
        view.frame++;
    }

    // Uploads the scene's changes, sets the uniforms and draws.
    inline void draw(projv::World& world, projv::graphics::RenderInstance& renderInstance, projv::GPUData& gpu,
                     projv::Scene& scene, vec3 position, vec3 direction, vec3 sunDirection) {
        projv::graphics::flushSceneUpdates(scene, gpu);
        upload(world, renderInstance.getActiveRenderer(), renderInstance.getWindowResolution(), position,
               direction, sunDirection);
        projv::graphics::renderConstructedRenderer(renderInstance, renderInstance.getActiveRenderer(), &gpu);
    }
}
