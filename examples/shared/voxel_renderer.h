// The examples' shared renderer (examples/shared/voxelRenderer/): a G-buffer (one primary and one sun
// shadow ray a render pixel), AO at two scales, the shade, the voxel reconstruction, TAA and the display.
// First written for Voxel Mayhem (examples/19-voxel-mayhem); plan: docs/plans/voxel-renderer.md.
//
//     projv_add_example(name ... RENDERER ../shared/voxelRenderer INCLUDE_DIRS ../shared)
//
//     voxel_renderer::install(world, renderInstance, here);              // at startup
//     voxel_renderer::draw(world, renderInstance, gpu, scene, eye, forward);   // each frame
//
// A program that adds passes of its own (a HUD) takes the specification from `specification`, extends
// it, and constructs it itself; then calls `upload` and renders each frame.
//
// Its look is the View in world.ctx(): what the passes do, the sun's height, and the scene's scale.
#pragma once

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

namespace voxel_renderer {
    using projv::core::vec3;
    using projv::core::vec4;

    struct View {
        bool  shadows = true, ambientOcclusion = true, taa = true;
        int   debugView = 0;       // 0 off, 1 reconstruction coverage, 2 AO only, 3 sun visibility
        float sun = 0.86f;         // the sun's angle up its arc
        bool  crosshair = false;   // two bars through the centre, for aiming
        float renderScale = 0.5f;  // of the window, each way: reconstruction and TAA rebuild the rest
        // The scene's scale: the finest voxel's edge (AO radii are in it), and the radius the haze is
        // measured over (8% lost across it).
        float finestVoxel = 0.125f;
        float hazeRadius = 150.0f;
        // Last frame's camera, for TAA.
        vec3  prevPosition{0.0f}, prevDirection{0.0f, 0.0f, 1.0f};
        bool  hasPrevious = false;
        int   frame = 0;
    };

    // The renderer staged beside the binary (`here`/`folder`), with its render-resolution targets scaled
    // by the View's render scale. Emplaces a View if there is none.
    inline projv::RendererSpecification specification(projv::World& world, const std::filesystem::path& here,
                                                      const std::string& folder = "voxelRenderer") {
        View& view = world.ctx().contains<View>() ? world.ctx().get<View>() : world.ctx().emplace<View>();
        projv::RendererSpecification spec = projv::graphics::loadRendererSpecification((here / folder).string() + "/");
        // Everything but what is at output resolution: the reconstruction and the TAA.
        for (projv::Texture& texture : spec.resources.textures)
            if (texture.sizeMode == projv::TextureSizeMode::Relative && texture.name != "reconstructed" && texture.name != "taaColor")
                texture.scale *= view.renderScale;
        return spec;
    }

    // Loads the renderer as `renderInstance`'s active renderer.
    inline void install(projv::World& world, projv::graphics::RenderInstance& renderInstance, const std::filesystem::path& here,
                        const std::string& folder = "voxelRenderer") {
        renderInstance.addRendererSpecification(1, specification(world, here, folder));
        bgfx::ShaderHandle vertexShader = projv::graphics::loadShader((here / folder / "shaders/vs_quad.bin").string());
        renderInstance.setActiveRenderer(
            projv::graphics::constructRendererSpecification(renderInstance.getRendererSpecification(1), vertexShader));
    }

    // Sets this frame's uniforms from the View and the camera.
    inline void upload(projv::World& world, std::shared_ptr<projv::ConstructedRenderer> renderer, vec3 position, vec3 direction) {
        using projv::graphics::setUniformToValue;
        View& view = world.ctx().get<View>();
        if (!view.hasPrevious) {
            view.prevPosition = position;
            view.prevDirection = direction;
            view.hasPrevious = true;
        }
        const bool moved = glm::length(position - view.prevPosition) > 1e-5f || glm::length(direction - view.prevDirection) > 1e-6f;
        vec3 prevPosition = view.prevPosition, prevDirection = view.prevDirection;
        setUniformToValue(renderer, "cameraPos", position);
        setUniformToValue(renderer, "cameraDir", direction);
        setUniformToValue(renderer, "prevCameraPos", prevPosition);
        setUniformToValue(renderer, "prevCameraDir", prevDirection);
        vec4 sun(std::cos(view.sun) * -0.6402f, std::sin(view.sun), std::cos(view.sun) * 0.7682f, view.crosshair ? 1.0f : 0.0f);
        setUniformToValue(renderer, "sunDir", sun);
        // Sun, sky, exposure, the finest voxel's edge.
        vec4 light(8.0f, 1.0f, 1.0f, view.finestVoxel);
        setUniformToValue(renderer, "lightParams", light);
        // Haze: 8% lost over the radius, thinning over a third of it up.
        vec4 fog(-std::log(0.92f) / view.hazeRadius, view.hazeRadius * 0.35f, 0.0f, 0.25f);
        setUniformToValue(renderer, "fogParams", fog);
        vec4 frameParams(float(view.frame++), 0.0f, view.taa ? 1.0f : 0.0f, moved ? 1.0f : 0.0f);
        setUniformToValue(renderer, "frameParams", frameParams);
        vec4 renderParams(view.shadows ? 1.0f : 0.0f, view.ambientOcclusion ? 1.0f : 0.0f, 0.0f, float(view.debugView));
        setUniformToValue(renderer, "renderParams", renderParams);
        view.prevPosition = position;
        view.prevDirection = direction;
    }
    inline void upload(projv::World& world, projv::graphics::RenderInstance& renderInstance, vec3 position, vec3 direction) {
        upload(world, renderInstance.getActiveRenderer(), position, direction);
    }

    // Uploads the scene's changes, sets the uniforms and draws.
    inline void draw(projv::World& world, projv::graphics::RenderInstance& renderInstance, projv::GPUData& gpu, projv::Scene& scene,
                     vec3 position, vec3 direction) {
        projv::graphics::flushSceneUpdates(scene, gpu);
        upload(world, renderInstance, position, direction);
        projv::graphics::renderConstructedRenderer(renderInstance, renderInstance.getActiveRenderer(), &gpu);
    }
}
