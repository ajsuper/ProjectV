// Exit-path test for the two fixes in the engine's application loop.
//
// Case A: an application that registers NO Shutdown stage still exits cleanly.
//         createApp() used to leave Application::Shutdown empty while runApplication()
//         called it unconditionally, so every such app threw std::bad_function_call
//         on the way out.
//
// Case B: RenderInstance::shouldClose reflects a window-manager close request, and an
//         application that reads it can end the loop. Nothing in the engine acts on the
//         flag -- this test is also the worked example of the hand-off.
//
//         The test never writes shouldClose itself. It drives the real per-frame entry point,
//         renderConstructedRenderer, which is the only place the engine polls GLFW and so the
//         only place the flag can be refreshed. If the engine stopped setting it, this hangs
//         and the watchdog below fails the test.
//
// The close request is delivered with glfwSetWindowShouldClose, which is exactly what
// GLFW's own WM handler calls when the titlebar X is clicked, so the path under test is
// the real one; only the source of the request is synthetic.
//
// Run:  ./exit_path a   (no Shutdown registered)
//       ./exit_path b   (close request honoured)
// Exits 0 on success, non-zero with a message on failure.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include "core/ecs.h"
#include "core/paths.h"
#include "graphics/render_instance.h"
#include "graphics/perform_renderer.h"
#include "graphics/disk_io.h"
#include "graphics/gpu_interface.h"
#include "graphics/manage_resources.h"

namespace {
    bool g_shutdownRan = false;
    int  g_framesBeforeClose = 10;

    // Borrows the ScenePreviewer's renderer so the test drives a real ConstructedRenderer
    // through the real submit path rather than a stand-in. The scene is left empty: this test
    // is about the loop, not about what is drawn.
    //
    // The renderer has to be loaded with the working directory set to the example's own folder.
    // Its resources.json names shader binaries as "./previewRenderer/previewShaders/albedo.bin"
    // -- paths inside the data, resolved against the process CWD -- so pointing the loader at
    // the folder from elsewhere finds the JSON and then fails on every shader it names.
    // executableDirectory() does not help here: the coupling is in the scene data, not the call.
    // Making renderer folders relocatable is its own change; this test just obeys the rule.
    const char* kExampleDir      = "../../docs/examples/ScenePreviewer";
    const char* kRendererDir     = "./previewRenderer/";
    const char* kVertexShaderBin = "./previewRenderer/previewShaders/vs_quad.bin";

    void startup(projv::Application& app) {
        std::error_code chdirError;
        std::filesystem::current_path(
            projv::core::executableDirectory() / kExampleDir, chdirError);
        if (chdirError) {
            std::fprintf(stderr, "FAIL: cannot enter %s: %s\n",
                         kExampleDir, chdirError.message().c_str());
            std::exit(5);
        }

        auto& ri = projv::core::createGlobalResource<projv::graphics::RenderInstance>(app.world);
        ri.initialize(320, 240, "ProjectV exit-path test");

        auto& scene   = projv::core::createGlobalResource<projv::Scene>(app.world);
        auto& gpuData = projv::core::createGlobalResource<projv::GPUData>(app.world);

        projv::RendererSpecification spec =
            projv::graphics::loadRendererSpecification(kRendererDir);
        ri.addRendererSpecification(1, spec);
        bgfx::ShaderHandle vsh = projv::graphics::loadShader(kVertexShaderBin);
        ri.setActiveRenderer(projv::graphics::constructRendererSpecification(
            ri.getRendererSpecification(1), vsh));

        gpuData = projv::graphics::createTexturesForScene(scene);
    }

    // Case B's hand-off: the engine records the request, the application decides what it means.
    void update(projv::Application& app) {
        auto& ri = projv::core::getGlobalResource<projv::graphics::RenderInstance>(app.world);

        // Before the request is delivered the flag must be false; a stuck-true flag would
        // make the rest of this test pass for the wrong reason.
        if (app.frameCount < g_framesBeforeClose && ri.shouldClose) {
            std::fprintf(stderr, "FAIL: shouldClose was true before any close request\n");
            std::exit(4);
        }

        if (app.frameCount == g_framesBeforeClose) {
            std::printf("[test] frame %d: delivering close request\n", app.frameCount);
            glfwSetWindowShouldClose(ri.window, GLFW_TRUE);
        }
        if (ri.shouldClose) {
            std::printf("[test] frame %d: shouldClose observed, ending loop\n", app.frameCount);
            app.closeAppFlag = true;
        }
        // A close request that is never observed would hang the test forever.
        if (app.frameCount > g_framesBeforeClose + 300) {
            std::fprintf(stderr, "FAIL: shouldClose never became true after the close request\n");
            std::exit(2);
        }
    }

    // The engine's per-frame entry point. It polls GLFW and, as of the exit-path fix, records
    // the close request on the RenderInstance. The test deliberately does not touch
    // shouldClose anywhere -- every observation of it comes from this call.
    void render(projv::Application& app) {
        auto& ri = projv::core::getGlobalResource<projv::graphics::RenderInstance>(app.world);
        auto& gpuData = projv::core::getGlobalResource<projv::GPUData>(app.world);
        projv::graphics::renderConstructedRenderer(ri, ri.getActiveRenderer(), &gpuData);
    }

    void shutdown(projv::Application&) {
        g_shutdownRan = true;
        std::printf("[test] Shutdown stage ran\n");
    }
}

int main(int argc, char** argv) {
    const std::string mode = (argc > 1) ? argv[1] : "b";

    projv::Application app = projv::core::createApp();
    projv::core::assignSystemStage(app, projv::SystemStage::Startup, startup);
    projv::core::assignSystemStage(app, projv::SystemStage::Update,  update);
    projv::core::assignSystemStage(app, projv::SystemStage::Render,  render);

    if (mode == "b") {
        projv::core::assignSystemStage(app, projv::SystemStage::Shutdown, shutdown);
    } else {
        std::printf("[test] case A: no Shutdown stage registered\n");
    }

    // Throws std::bad_function_call here, before the fix, whenever mode == "a".
    projv::core::runApplication(app);

    if (mode == "b" && !g_shutdownRan) {
        std::fprintf(stderr, "FAIL: registered Shutdown stage never ran\n");
        return 3;
    }
    std::printf("PASS (%s)\n", mode.c_str());
    return 0;
}
