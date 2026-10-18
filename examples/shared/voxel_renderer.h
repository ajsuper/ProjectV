// The examples' shared renderer (examples/shared/voxelRenderer/): ten passes -- a G-buffer,
// screen-space AO and colour bounce, one-bounce ray-traced GI at a sixteenth of the render pixels
// with its own history and blur, the shade with sun shadows, the voxel reconstruction, TAA and the
// display. First written for Voxel Mayhem (examples/19-voxel-mayhem).
//
//     projv_add_example(name ... RENDERER ../shared/voxelRenderer INCLUDE_DIRS ../shared)
//
//     voxel_renderer::install(world, renderInstance, here);              // at startup
//     voxel_renderer::draw(world, renderInstance, gpu, eye, forward);    // each frame
//
// Its look is the View in world.ctx(): what the passes do, the sun's height, and the scene's scale.
#pragma once

#include <cmath>
#include <filesystem>

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
        int   bounce = 1;          // the colour bounce: off, normal, strong
        bool  rayGI = true;        // the traced large-scale light
        int   debugView = 0;       // 0 off, 2 AO only, 4 the bounce only, 7 the ray GI only
        float sun = 0.86f;         // the sun's angle up its arc
        bool  crosshair = false;   // two bars through the centre, for aiming
        float renderScale = 0.5f;  // of the window, each way: reconstruction and TAA rebuild the rest
        // The scene's scale: the finest voxel's edge (AO and GI radii are in it), and the radius the
        // haze is measured over (8% lost across it).
        float finestVoxel = 0.125f;
        float hazeRadius = 150.0f;
        // Last frame's camera, for TAA and the GI history.
        vec3  prevPosition{0.0f}, prevDirection{0.0f, 0.0f, 1.0f};
        bool  hasPrevious = false;
        int   frame = 0;
    };

    // Loads the renderer staged beside the binary (`here`/voxelRenderer) into `renderInstance` as its
    // active renderer, at the View's render scale. Emplaces a View if there is none.
    inline void install(projv::World& world, projv::graphics::RenderInstance& renderInstance, const std::filesystem::path& here) {
        View& view = world.ctx().contains<View>() ? world.ctx().get<View>() : world.ctx().emplace<View>();
        projv::RendererSpecification specification =
            projv::graphics::loadRendererSpecification((here / "voxelRenderer").string() + "/");
        for (projv::Texture& texture : specification.resources.textures)
            if (texture.sizeMode == projv::TextureSizeMode::Relative && texture.name != "reconstructed" && texture.name != "taaColor")
                texture.scale *= view.renderScale;
        renderInstance.addRendererSpecification(1, specification);
        bgfx::ShaderHandle vertexShader = projv::graphics::loadShader((here / "voxelRenderer/shaders/vs_quad.bin").string());
        renderInstance.setActiveRenderer(
            projv::graphics::constructRendererSpecification(renderInstance.getRendererSpecification(1), vertexShader));
    }

    // Sets this frame's uniforms from the View and the camera.
    inline void upload(projv::World& world, projv::graphics::RenderInstance& renderInstance, vec3 position, vec3 direction) {
        using projv::graphics::setUniformToValue;
        auto renderer = renderInstance.getActiveRenderer();
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
        static const float bounceStrength[3] = {0.0f, 2.5f, 5.0f};
        vec4 giParams(view.ambientOcclusion ? bounceStrength[view.bounce] : 0.0f, view.rayGI ? 1.0f : 0.0f, 0.0f, 0.0f);
        setUniformToValue(renderer, "giParams", giParams);
        view.prevPosition = position;
        view.prevDirection = direction;
    }

    // Uploads the scene's changes, sets the uniforms and draws.
    inline void draw(projv::World& world, projv::graphics::RenderInstance& renderInstance, projv::GPUData& gpu, projv::Scene& scene,
                     vec3 position, vec3 direction) {
        projv::graphics::flushSceneUpdates(scene, gpu);
        upload(world, renderInstance, position, direction);
        projv::graphics::renderConstructedRenderer(renderInstance, renderInstance.getActiveRenderer(), &gpu);
    }
}
