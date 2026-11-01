// Exit-path test for the two fixes in the engine's application loop.
//
// Case A: an application that registers NO Shutdown stage still exits cleanly.
//         The original loop left Application::Shutdown empty while the run loop called it
//         unconditionally, so every such app threw std::bad_function_call on the way out.
//         (Application now runs a list of systems per stage, so an empty stage is simply
//         empty -- tests/unit/test_application.cpp covers it without a window. This harness
//         stays for the windowed half, Case B.)
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
#include <string>

#include "core/application.h"
#include "graphics/basic_renderer.h"
#include "graphics/render_instance.h"
#include "graphics/perform_renderer.h"
#include "graphics/gpu_interface.h"

namespace {
    bool g_shutdownRan = false;
    int  g_framesBeforeClose = 10;

    // Draws with the engine's basic renderer, so the test drives a real ConstructedRenderer through
    // the real submit path rather than a stand-in. The scene is left empty: this test is about the
    // loop, not about what is drawn.

    void startup(projv::Application& app) {
        auto& ri = app.world.ctx().emplace<projv::graphics::RenderInstance>();
        ri.initialize(320, 240, "ProjectV exit-path test");

        auto& scene   = app.world.ctx().emplace<projv::Scene>();
        auto& gpuData = app.world.ctx().emplace<projv::GPUData>();

        ri.setActiveRenderer(projv::graphics::createBasicRenderer());

        gpuData = projv::graphics::createTexturesForScene(scene);
    }

    // Case B's hand-off: the engine records the request, the application decides what it means.
    void update(projv::Application& app) {
        auto& ri = app.world.ctx().get<projv::graphics::RenderInstance>();

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
            app.closeRequested = true;
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
        auto& ri = app.world.ctx().get<projv::graphics::RenderInstance>();
        auto& gpuData = app.world.ctx().get<projv::GPUData>();
        // drawBasicRenderer ends in renderConstructedRenderer.
        projv::graphics::drawBasicRenderer(ri, ri.getActiveRenderer(), gpuData, projv::CameraView{});
    }

    void shutdown(projv::Application&) {
        g_shutdownRan = true;
        std::printf("[test] Shutdown stage ran\n");
    }
}

int main(int argc, char** argv) {
    const std::string mode = (argc > 1) ? argv[1] : "b";

    projv::Application app;
    app.addSystem(projv::Stage::Startup, "startup", startup);
    app.addSystem(projv::Stage::Update, "update", update);
    app.addSystem(projv::Stage::Render, "render", render);

    if (mode == "b") {
        app.addSystem(projv::Stage::Shutdown, "shutdown", shutdown);
    } else {
        std::printf("[test] case A: no Shutdown stage registered\n");
    }

    // Threw std::bad_function_call here, in the original loop, whenever mode == "a".
    app.run();

    if (mode == "b" && !g_shutdownRan) {
        std::fprintf(stderr, "FAIL: registered Shutdown stage never ran\n");
        return 3;
    }
    std::printf("PASS (%s)\n", mode.c_str());
    return 0;
}
