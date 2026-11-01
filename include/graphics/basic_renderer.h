#ifndef PROJV_GRAPHICS_BASIC_RENDERER_H
#define PROJV_GRAPHICS_BASIC_RENDERER_H

#include <memory>

#include "core/math.h"
#include "data_structures/camera_view.h"
#include "data_structures/constructedRenderer.h"
#include "data_structures/gpuData.h"

// The engine's basic renderer: a way to see a scene without writing a renderer. One primary ray a
// pixel, the voxel's colour under a sun and a sky, drawn to the window. Its shader is compiled into
// the library (shaders/engine/basic.frag), so nothing is staged beside the program.
//
//     projv::graphics::installBasicRenderer(app, renderInstance);           // at startup, after
//                                                                           // the Scene and GPUData
//     app.world.ctx().get<projv::CameraView>() = view;                      // each frame
//
// installBasicRenderer adds a Render system, "basic renderer", that uploads the Scene's changes to
// the GPUData (both from world.ctx()) and draws from the CameraView it keeps there. A program that
// draws by hand uses createBasicRenderer and drawBasicRenderer instead.
//
// Plan: docs/plans/shader-library.md, D6.
namespace projv {
    class Application;
}

namespace projv::graphics {
    class RenderInstance;

    /**
     * The basic renderer's look. The defaults are neutral: a high sun, a grey-blue sky.
     */
    struct BasicRendererOptions {
        core::vec3 sunDirection{0.35f, 0.85f, 0.4f};    ///< Toward the sun; normalised when drawn.
        core::vec3 sunColour{0.75f, 0.75f, 0.75f};      ///< Linear.
        core::vec3 ambientAbove{0.4f, 0.4f, 0.4f};      ///< Linear light on a face turned straight up,
        core::vec3 ambientBelow{0.2f, 0.2f, 0.2f};      ///< and straight down; mixed by the normal.
        core::vec3 skyAbove{0.42f, 0.55f, 0.72f};       ///< The sky straight up, sRGB (as picked),
        core::vec3 skyBelow{0.10f, 0.11f, 0.14f};       ///< and straight down.
        /**
         * Two bars through the centre, for aiming. A crosshair is UI; it is here only until a
         * program can draw its own (docs/plans/ui.md), and goes then.
         */
        bool crosshair = false;
    };

    /**
     * The world.ctx() resource installBasicRenderer keeps: the renderer and the look it draws with.
     * Change `options` at any time; the next frame uses them.
     */
    struct BasicRenderer {
        std::shared_ptr<ConstructedRenderer> renderer;
        BasicRendererOptions options;
    };

    /**
     * Builds the basic renderer from the library's embedded shaders. bgfx must be initialised.
     * @return The renderer, or nullptr (logged) when the library has no basic renderer for the
     *         running renderer type.
     */
    std::shared_ptr<ConstructedRenderer> createBasicRenderer();

    /**
     * Draws one frame from `camera` and submits it (bgfx::frame). Uploads nothing: flush the scene's
     * changes first (flushSceneUpdates).
     */
    void drawBasicRenderer(RenderInstance& renderInstance, const std::shared_ptr<ConstructedRenderer>& renderer,
                           GPUData& gpuData, const CameraView& camera, const BasicRendererOptions& options = {});

    /**
     * Creates the basic renderer, makes it the active renderer, and adds the "basic renderer" Render
     * system: each frame it flushes the Scene into the GPUData and draws from the CameraView, all
     * three from world.ctx(). Emplaces a BasicRenderer, and a CameraView if there is none. Call it
     * once bgfx is up (in Startup, after RenderInstance::initialize). A Render system of the
     * program's own that must run after the frame is drawn orders itself {.after = "basic renderer"}.
     * @return False (logged) when the renderer could not be created; nothing is added then.
     */
    bool installBasicRenderer(Application& app, RenderInstance& renderInstance, BasicRendererOptions options = {});
}

#endif
