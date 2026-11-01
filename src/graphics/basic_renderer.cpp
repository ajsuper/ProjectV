#include "graphics/basic_renderer.h"

#include <span>

#include "core/application.h"
#include "core/log.h"
#include "data_structures/scene.h"
#include "graphics/camera.h"
#include "graphics/engine_shaders.h"
#include "graphics/gpu_interface.h"
#include "graphics/manage_resources.h"
#include "graphics/perform_renderer.h"
#include "graphics/render_instance.h"

namespace projv::graphics {
    namespace {
        // The uniforms basic.frag declares, in its order.
        const char* const basicUniformNames[] = {"pjvBasicSun",          "pjvBasicSunColour", "pjvBasicAmbientAbove",
                                                 "pjvBasicAmbientBelow", "pjvBasicSkyAbove",  "pjvBasicSkyBelow",
                                                 "pjvBasicOptions"};
    }

    std::shared_ptr<ConstructedRenderer> createBasicRenderer() {
        std::span<const uint8_t> fragment = engineShader("basic");
        if (fragment.empty()) {
            core::error("No basic renderer for renderer {}", bgfx::getRendererName(bgfx::getRendererType()));
            return nullptr;
        }
        // What a renderer folder's resources.json and render.json would say: one shader, one pass to
        // the back buffer, no textures or framebuffers.
        RendererSpecification specification;
        for (const char* name : basicUniformNames) {
            specification.resources.uniforms.push_back(Uniform{0, UniformType::Vec4, name});
        }
        Shader shader;
        shader.shaderID = 1;
        shader.shaderFileContents.assign(fragment.begin(), fragment.end());
        specification.resources.shaders.push_back(std::move(shader));
        specification.dependencyGraph.renderPasses.push_back(RenderPass{1, {}, {}, -1, 0, 0});
        return constructRendererSpecification(specification, loadEngineShader("vs_quad"));
    }

    void drawBasicRenderer(RenderInstance& renderInstance, const std::shared_ptr<ConstructedRenderer>& renderer,
                           GPUData& gpuData, const CameraView& camera, const BasicRendererOptions& options) {
        // Locals: setUniformToValue dispatches on its argument's exact, non-const type.
        core::vec4 values[] = {
            core::vec4(glm::normalize(options.sunDirection), 0.0f),
            core::vec4(options.sunColour, 0.0f),
            core::vec4(options.ambientAbove, 0.0f),
            core::vec4(options.ambientBelow, 0.0f),
            core::vec4(options.skyAbove, 0.0f),
            core::vec4(options.skyBelow, 0.0f),
            core::vec4(options.crosshair ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f),
        };
        for (size_t i = 0; i < std::size(values); i++) {
            setUniformToValue(renderer, basicUniformNames[i], values[i]);
        }
        setCamera(*renderer, camera);
        renderConstructedRenderer(renderInstance, renderer, &gpuData);
    }

    bool installBasicRenderer(Application& app, RenderInstance& renderInstance, BasicRendererOptions options) {
        std::shared_ptr<ConstructedRenderer> renderer = createBasicRenderer();
        if (!renderer) return false;
        renderInstance.setActiveRenderer(renderer);
        app.world.ctx().insert_or_assign(BasicRenderer{renderer, options});
        if (!app.world.ctx().contains<CameraView>()) app.world.ctx().emplace<CameraView>();

        app.addSystem(Stage::Render, "basic renderer", [&renderInstance](Application& app) {
            auto& context = app.world.ctx();
            GPUData* gpuData = context.find<GPUData>();
            if (!gpuData) {
                static bool warned = false;
                if (!warned) core::error("basic renderer: no GPUData in world.ctx(), so nothing is drawn");
                warned = true;
                return;
            }
            if (Scene* scene = context.find<Scene>()) flushSceneUpdates(*scene, *gpuData);
            const BasicRenderer& basic = context.get<BasicRenderer>();
            drawBasicRenderer(renderInstance, basic.renderer, *gpuData, context.get<CameraView>(), basic.options);
        });
        return true;
    }
}
