// restir_test.h -- an experiment: Voxel Mayhem drawn by the renderer gallery's restir-pt instead of the
// demos' voxel renderer. MAYHEM_RENDERER=restir-pt turns it on; nothing else changes.
//
// restir-pt's folder is the gallery's, loaded from a `renderers/` directory beside this binary (its
// resources.json names its shaders relative to the working directory, so this enters the binary's
// directory first). The build does not stage it here: link or copy build/examples/renderer_gallery/renderers
// next to voxel_mayhem_demo to try it. The uniforms are the gallery's restir-pt defaults (main.cpp there),
// fed from Mayhem's camera and sun; its HUD and crosshair are the voxel renderer's, and are not drawn.
//
// Moving bodies: the G-buffer's motion vectors come from each chunk's pose last frame (the voxel renderer's
// prevPoses upload, into restir-pt's texture 99), so the resolve and the motion blur follow them. The GI's
// own histories still follow the camera alone: a moving body's indirect light lags a few frames.

#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "graphics/disk_io.h"
#include "graphics/manage_resources.h"
#include "graphics/perform_renderer.h"
#include "graphics/render_instance.h"
#include "voxel_renderer.h"

namespace restir_test {

    inline bool wanted() {
        const char* v = std::getenv("MAYHEM_RENDERER");
        return v && std::string(v) == "restir-pt";
    }

    struct State {
        int frame = 0;
        bool has = false;
        glm::vec3 prevPosition{0.0f}, prevDirection{0.0f, 0.0f, 1.0f};
        float prevSun = 0.0f;
        float ground = 0.0f;   // the fog's ground height: Mayhem's floor is y = 0, a map's is its lowest point
        std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    };

    inline bool install(projv::graphics::RenderInstance& renderInstance, const std::filesystem::path& here) {
        std::error_code error;
        std::filesystem::current_path(here, error);
        if (!std::filesystem::exists("renderers/restir-pt/render.json")) {
            projv::core::error("restir test: no renderers/restir-pt beside the binary -- link the gallery's renderers/ here");
            return false;
        }
        renderInstance.addRendererSpecification(1, projv::graphics::loadRendererSpecification("./renderers/restir-pt/"));
        bgfx::ShaderHandle vs = projv::graphics::loadShader("./renderers/restir-pt/shaders/vs_quad.bin");
        renderInstance.setActiveRenderer(projv::graphics::constructRendererSpecification(renderInstance.getRendererSpecification(1), vs));
        projv::core::info("restir test: drawing with the gallery's restir-pt");
        return true;
    }

    // setUniformToValue takes a reference; these are all temporaries.
    inline void set(std::shared_ptr<projv::ConstructedRenderer> renderer, const char* name, glm::vec4 value) {
        projv::graphics::setUniformToValue(renderer, name, value);
    }

    // The gallery's restir-pt uniforms (main.cpp, uploadRestirUniforms) at their defaults.
    inline void upload(State& st, projv::World& world, std::shared_ptr<projv::ConstructedRenderer> renderer, glm::vec2 window, glm::vec3 position,
                       glm::vec3 direction, float sunAngle, float finest, float sceneRadius) {
        using glm::vec4;
        if (!st.has) {
            st.prevPosition = position;
            st.prevDirection = direction;
            st.prevSun = sunAngle;
            st.has = true;
        }
        const bool sunMoved = sunAngle != st.prevSun;
        const bool moved = sunMoved || glm::length(position - st.prevPosition) > 1e-5f || glm::length(direction - st.prevDirection) > 1e-6f;
        const int frame = st.frame;
        static const int order4[4][2] = {{0, 0}, {1, 1}, {1, 0}, {0, 1}};
        auto halton = [](int index, int base) {
            float f = 1.0f, r = 0.0f;
            for (; index > 0; index /= base) { f /= float(base); r += f * float(index % base); }
            return r;
        };
        const float pixelWidth = 2.0f * std::tan(30.0f * 3.14159265f / 180.0f) / std::max(window.y * 0.5f, 1.0f);
        const float fogLength = std::clamp(1.6f * sceneRadius, 300.0f, 3000.0f);
        const auto now = std::chrono::steady_clock::now();
        const float seconds = std::chrono::duration<float>(now - st.last).count();
        st.last = now;

        set(renderer, "cameraPos", vec4(position, 1.0f));
        set(renderer, "cameraDir", vec4(direction, 0.0f));
        set(renderer, "prevCameraPos", vec4(st.prevPosition, 1.0f));
        set(renderer, "prevCameraDir", vec4(st.prevDirection, 0.0f));
        set(renderer, "sunDir", vec4(std::cos(sunAngle) * -0.6402f, std::sin(sunAngle), std::cos(sunAngle) * 0.7682f, 0.0f));
        set(renderer, "frameCount", vec4(float(frame), moved ? 1.0f : 0.0f, sunMoved ? 1.0f : 0.0f, 0.0f));
        set(renderer, "restirReuse", vec4(1.0f, 1.0f, 1.0f, 4.0f));
        set(renderer, "restirDenoise", vec4(1.0f, 1.0f, 0.0f, 12.0f));
        set(renderer, "restirJitter", vec4(float(order4[frame % 4][0]), float(order4[frame % 4][1]), 2.0f, frame == 0 ? 1.0f : 0.0f));
        set(renderer, "restirSettings", vec4(6.0f, finest * 0.05f, sunMoved ? 1.0f : moved ? 16.0f : 64.0f, pixelWidth));
        set(renderer, "restirLook", vec4(6.0f * finest, 0.85f, 2.0f * std::tan(0.02f), 0.0f));
        set(renderer, "restirHaze", vec4(0.6f / fogLength, st.ground, 0.15f * fogLength, 0.9f));
        set(renderer, "restirPath", vec4(2.0f, 12.0f, 8.0f, 0.0f));
        set(renderer, "jitterParams", vec4(halton(frame % 16 + 1, 2) - 0.5f, halton(frame % 16 + 1, 3) - 0.5f, 0.0f, 0.0f));
        set(renderer, "renderParams", vec4(0.0f));
        set(renderer, "restirPost", vec4(0.15f, 1.0f, 1.0f, seconds));
        set(renderer, "restirDof", vec4(1.5f * window.y / 1080.0f, 0.04f, 0.5f, 12.0f * window.y / 1080.0f));
        voxel_renderer::uploadPoses(world, renderer, 99);
        set(renderer, "restirShafts", vec4(0.0f, 1.0f / (48.0f * finest), 0.6f, 16.0f));

        st.prevPosition = position;
        st.prevDirection = direction;
        st.prevSun = sunAngle;
        st.frame++;
    }

}
