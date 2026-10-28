// The feature examples' renderer (examples/shared/basicRenderer/): one primary ray a pixel, the
// voxel's colour lit by a fixed sun and the sky. Nothing else -- the examples are about what the
// engine does, and the _demo examples are where the looks are. Hello Voxel spells the same steps out
// by hand; every other feature example calls these two.
//
//     projv_add_example(name ... RENDERER ../shared/basicRenderer INCLUDE_DIRS ../shared)
//
//     basic_renderer::install(renderInstance, here);                         // at startup
//     basic_renderer::draw(renderInstance, gpu, scene, eye, forward);        // each frame
#pragma once

#include <filesystem>
#include <string>

#include "core/math.h"
#include "graphics/disk_io.h"
#include "graphics/engine_shaders.h"
#include "graphics/gpu_interface.h"
#include "graphics/manage_resources.h"
#include "graphics/perform_renderer.h"
#include "graphics/render_instance.h"

namespace basic_renderer {
    struct Options {
        bool crosshair = false;   // two bars through the centre, for aiming
    };

    // Loads the renderer staged beside the binary (`here`/basicRenderer) as the active renderer.
    inline void install(projv::graphics::RenderInstance& renderInstance, const std::filesystem::path& here) {
        projv::RendererSpecification specification =
            projv::graphics::loadRendererSpecification((here / "basicRenderer").string() + "/");
        renderInstance.addRendererSpecification(1, specification);
        bgfx::ShaderHandle vertexShader = projv::graphics::loadEngineShader("vs_quad");
        renderInstance.setActiveRenderer(
            projv::graphics::constructRendererSpecification(renderInstance.getRendererSpecification(1), vertexShader));
    }

    // Sets the uniforms and draws, from `position` looking along `direction`. Uploads nothing: for a
    // program that flushes the scene's changes itself.
    inline void render(projv::graphics::RenderInstance& renderInstance, projv::GPUData& gpu,
                       projv::core::vec3 position, projv::core::vec3 direction, Options options = {}) {
        using projv::graphics::setUniformToValue;
        auto renderer = renderInstance.getActiveRenderer();
        // Locals, not the arguments' members: setUniformToValue dispatches on its argument's exact type.
        projv::core::vec2 resolution = renderInstance.getWindowResolution();
        projv::core::vec4 view(options.crosshair ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
        setUniformToValue(renderer, "cameraPos", position);
        setUniformToValue(renderer, "cameraDir", direction);
        setUniformToValue(renderer, "windowRes", resolution);
        setUniformToValue(renderer, "viewOptions", view);
        projv::graphics::renderConstructedRenderer(renderInstance, renderer, &gpu);
    }

    // Uploads the scene's changes, then draws.
    inline void draw(projv::graphics::RenderInstance& renderInstance, projv::GPUData& gpu, projv::Scene& scene,
                     projv::core::vec3 position, projv::core::vec3 direction, Options options = {}) {
        projv::graphics::flushSceneUpdates(scene, gpu);
        render(renderInstance, gpu, position, direction, options);
    }
}
