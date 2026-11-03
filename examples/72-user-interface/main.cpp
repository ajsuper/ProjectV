// ProjectV User Interface
//
// The UI's user layer (docs/plans/ui.md, U3): what a player sees, as RmlUi documents beside the
// program -- ui/hud.rml and ui/menu.rml, styled by the demos' shared base.rcss and this program's
// ui.rcss -- drawn over a scene. It shows the three things a game does with its UI:
//
//   * Values in. The program binds its own variables to a data model ("hud"): the frame rate, a click
//     count, a name. The documents show them ({{fps}}) and redraw them when they change.
//   * Actions out. A button's onclick="count 1" arrives as a ui::UiAction event, handled in Update
//     like any other event.
//   * Controls bound to values. The menu's slider, checkbox and text field write the variables they
//     are bound to (data-value, data-checked): the sun's height, the crosshair, the name.
//
// Edit ui/*.rml or ui/ui.rcss in the build folder while it runs: the documents reload (a dev build).
//
// UI_CAPTURE=<file>: the menu open (UI_CAPTURE_MENU=0: closed), frame 90 (UI_CAPTURE_AT=n: frame n)
// saved as <file>.tga, then it exits (an unattended look).
//
// Controls:
//   M        -- the menu (frees the cursor; Resume or M again to go back)
//   W/S/A/D  -- move;  R/F up / down;  mouse -- look
//   F1       -- the dev layer

#include <cmath>
#include <cstdlib>
#include <string>

#include <bgfx/bgfx.h>

#include "core/application.h"
#include "core/log.h"
#include "core/math.h"
#include "core/paths.h"
#include "graphics/basic_renderer.h"
#include "graphics/gpu_interface.h"
#include "graphics/input.h"
#include "graphics/render_instance.h"
#include "ui/ui.h"
#include "ui/user.h"
#include "utils/blob_voxels.h"
#include "utils/material.h"
#include "utils/voxel_management.h"

namespace {
    // What the documents show and the menu's controls write: the program's own state, bound by name.
    struct UiState {
        int fps = 0;
        int clicks = 0;
        std::string name = "player";
        float sunHeight = 0.85f;
        bool crosshair = false;
        bool menuOpen = false;
    };

    struct Camera {
        projv::core::vec3 position{-4.0f, 17.0f, -6.0f};
        float yaw = 0.9f;
        float pitch = -0.42f;
    };

    // A floor and a few pillars, so the UI has something to sit over. One loose 64^3 chunk, as in
    // Hello Voxel.
    projv::Scene buildScene() {
        constexpr int N = 64;
        projv::Scene scene;
        projv::Chunk chunk;
        chunk.header.chunkID = 1;
        chunk.header.position = projv::core::vec3(0.0f);
        chunk.header.scale = N * 0.5f;
        chunk.header.voxelScale = 0.5f;
        chunk.header.resolution = N;
        chunk.header.rotation = projv::core::quat(1.0f, 0.0f, 0.0f, 0.0f);
        chunk.alive = true;
        chunk.componentHandle = 0;
        scene.components.push_back(projv::ComponentRecord{projv::ComponentKind::Chunk, 0, -1, "internal/ui", false, {}, -1});
        projv::ComponentRecord& component = scene.components[0];
        const uint8_t floorA = projv::utils::internMaterial(scene, component, "floorA", projv::packColor({70, 78, 92}));
        const uint8_t floorB = projv::utils::internMaterial(scene, component, "floorB", projv::packColor({92, 100, 116}));
        const uint8_t pillar = projv::utils::internMaterial(scene, component, "pillar", projv::packColor({210, 150, 80}));
        auto bricks = projv::utils::createVoxelBrickMap(projv::utils::computeBrickDims(N));
        for (int z = 0; z < N; z++)
            for (int x = 0; x < N; x++)
                projv::utils::brickMapSetVoxel(*bricks, x, 0, z, ((x / 8 + z / 8) % 2) ? floorA : floorB);
        for (int p = 0; p < 5; p++) {
            const int px = 10 + p * 10, pz = 20 + (p % 2) * 18, height = 10 + p * 6;
            for (int y = 1; y < height; y++)
                for (int z = pz; z < pz + 5; z++)
                    for (int x = px; x < px + 5; x++) projv::utils::brickMapSetVoxel(*bricks, x, y, z, pillar);
        }
        projv::utils::updateChunkFromBrickMap(chunk, *bricks);
        std::vector<uint8_t> materials;
        projv::utils::bakeMaterialsFromBrickMap(chunk.geometryData, materials, *bricks);
        const int32_t blob = projv::internChunkGeometry(scene, chunk, std::move(bricks));
        if (blob >= 0) projv::utils::setBlobTreeMaterials(scene.geometryPool[size_t(blob)], std::move(materials));
        scene.chunks.push_back(std::move(chunk));
        scene.looseChunks.push_back(0);
        scene.looseChunkCount = 1;
        return scene;
    }

    void setMenuOpen(projv::Application& app, bool open) {
        auto& state = app.world.ctx().get<UiState>();
        auto& renderInstance = app.world.ctx().get<projv::graphics::RenderInstance>();
        state.menuOpen = open;
        if (open) projv::ui::user::show(app, "menu");
        else projv::ui::user::hide(app, "menu");
        projv::graphics::setCursorCaptured(app, renderInstance, !open);   // the menu needs the cursor
    }

    void startup(projv::Application& app) {
        auto& renderInstance = app.world.ctx().emplace<projv::graphics::RenderInstance>();
        renderInstance.initialize(1280, 720, "ProjectV User Interface");
        projv::graphics::installPlatform(app, renderInstance);
        projv::ui::installUi(app, renderInstance);
        projv::graphics::setCursorCaptured(app, renderInstance, true);

        auto& scene = app.world.ctx().emplace<projv::Scene>(buildScene());
        app.world.ctx().emplace<Camera>();
        projv::graphics::installBasicRenderer(app, renderInstance);
        app.world.ctx().emplace<projv::GPUData>(projv::graphics::createTexturesForScene(scene));

        // The UI: a font, the model the documents read (bound before they are opened), the documents.
        const std::string dir = projv::core::executableDirectory().string();
        auto& state = app.world.ctx().emplace<UiState>();
        projv::ui::user::loadFont(app, dir + "/shared_ui/fonts/LatoLatin-Regular.ttf");
        projv::ui::user::loadFont(app, dir + "/shared_ui/fonts/LatoLatin-Bold.ttf");
        projv::ui::user::model(app, "hud")
            .bind("fps", &state.fps)
            .bind("clicks", &state.clicks)
            .bind("name", &state.name)
            .bind("sunHeight", &state.sunHeight)
            .bind("crosshair", &state.crosshair)
            .bind("menuOpen", &state.menuOpen);
        projv::ui::user::open(app, "hud", dir + "/ui/hud.rml");
        projv::ui::user::open(app, "menu", dir + "/ui/menu.rml", {.shown = false, .fadeOut = 0.4f});   // ui.rcss fades it
        projv::ui::user::open(app, "crosshair", dir + "/shared_ui/crosshair.rml", {.shown = false});

        // The menu's buttons. Handled in Update, after the frame's input: a UI action is input.
        app.events().on<projv::ui::UiAction>([&app](const projv::ui::UiAction& action) {
            auto& s = app.world.ctx().get<UiState>();
            if (action.name == "resume") setMenuOpen(app, false);
            else if (action.name == "count") s.clicks += action.args.empty() ? 1 : std::atoi(action.args[0].c_str());
            else if (action.name == "quit") app.closeRequested = true;
            projv::core::info("ui: {} from {}", action.name, action.document);
        });
    }

    void update(projv::Application& app) {
        const auto& input = app.world.ctx().get<projv::Input>();
        auto& state = app.world.ctx().get<UiState>();
        auto& camera = app.world.ctx().get<Camera>();
        auto& renderInstance = app.world.ctx().get<projv::graphics::RenderInstance>();

        // M opens and closes the menu -- unless a text field has the keyboard (the name).
        if (!input.wantCaptureKeyboard && input.pressed(projv::Key::M)) setMenuOpen(app, !state.menuOpen);
        // Off the menu, Esc frees the cursor and a click on the scene takes it back, as everywhere.
        if (!state.menuOpen) {
            if (input.cursorCaptured && input.pressed(projv::Key::Escape)) projv::graphics::setCursorCaptured(app, renderInstance, false);
            else if (!input.cursorCaptured && !input.wantCaptureMouse && input.pressed(projv::MouseButton::Left))
                projv::graphics::setCursorCaptured(app, renderInstance, true);
        }

        if (input.cursorCaptured) {
            camera.yaw += input.cursorDelta.x * 0.0025f;
            camera.pitch = std::fmax(-1.55f, std::fmin(1.55f, camera.pitch - input.cursorDelta.y * 0.0025f));
        }
        const projv::core::vec3 forward{std::cos(camera.yaw), 0.0f, std::sin(camera.yaw)};
        const projv::core::vec3 right{-std::sin(camera.yaw), 0.0f, std::cos(camera.yaw)};
        const float speed = 24.0f * app.time().delta;
        if (!input.wantCaptureKeyboard) {
            if (input.down(projv::Key::W)) camera.position += forward * speed;
            if (input.down(projv::Key::S)) camera.position -= forward * speed;
            if (input.down(projv::Key::D)) camera.position += right * speed;
            if (input.down(projv::Key::A)) camera.position -= right * speed;
            if (input.down(projv::Key::R)) camera.position.y += speed;
            if (input.down(projv::Key::F)) camera.position.y -= speed;
        }

        // The values the HUD shows, and the ones the menu's controls wrote.
        static float fpsTimer = 0.0f;
        static int frames = 0;
        fpsTimer += app.time().unscaledDelta;
        frames++;
        if (fpsTimer >= 0.5f) {
            state.fps = int(std::lround(frames / fpsTimer));
            fpsTimer = 0.0f;
            frames = 0;
        }
        auto& basic = app.world.ctx().get<projv::graphics::BasicRenderer>();
        basic.options.sunDirection = {0.35f * std::cos(state.sunHeight), std::sin(state.sunHeight), 0.4f * std::cos(state.sunHeight)};
        static bool shownCrosshair = false;   // the shared crosshair document, shown as the checkbox says
        if (state.crosshair != shownCrosshair) {
            if (state.crosshair) projv::ui::user::show(app, "crosshair");
            else projv::ui::user::hide(app, "crosshair");
            shownCrosshair = state.crosshair;
        }

        if (const char* capture = std::getenv("UI_CAPTURE")) {
            const char* menu = std::getenv("UI_CAPTURE_MENU");
            if (app.frameCount == 2 && !(menu && std::string(menu) == "0")) setMenuOpen(app, true);
            static const int at = std::getenv("UI_CAPTURE_AT") ? std::atoi(std::getenv("UI_CAPTURE_AT")) : 90;
            if (app.frameCount == at) bgfx::requestScreenShot(BGFX_INVALID_HANDLE, capture);
            if (app.frameCount == at + 5) app.closeRequested = true;
        }

        auto& view = app.world.ctx().get<projv::CameraView>();
        view.position = camera.position;
        view.forward = {std::cos(camera.pitch) * std::cos(camera.yaw), std::sin(camera.pitch), std::cos(camera.pitch) * std::sin(camera.yaw)};
    }
}

int main(int argc, char** argv) {
    projv::ui::setCommandLine(argc, argv);
    projv::Application app;
    app.addSystem(projv::Stage::Startup, "startup", startup);
    app.addSystem(projv::Stage::Update, "update", update);
    app.run();
    return 0;
}
