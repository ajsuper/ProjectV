// The renderer gallery's restir-pt (examples/30-renderers/renderers/restir-pt/): its per-frame uniforms,
// for every program that draws with it -- the gallery, with its keys and switches, and Voxel Mayhem and
// the terrain generator, at the defaults -- so they draw it the same way. ReSTIR GI at a sixteenth (or a
// quarter) of the window's pixels, a G-buffer at half resolution each way rebuilt to the window by the
// voxel renderer's reconstruction, then a temporal resolve and the post (its README).
//
//     restir_pt::install(renderInstance, slot);            // at startup, from the binary's directory
//     restir_pt::upload(renderer, frame, scene, settings, &world);   // each frame
//
// The folder is the gallery's, staged beside each binary that uses it as renderers/restir-pt/ (its
// resources.json names its shaders there); the gallery compiles its shaders.
#pragma once

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>

#include "core/log.h"
#include "core/math.h"
#include "core/world.h"
#include "graphics/disk_io.h"
#include "graphics/manage_resources.h"
#include "graphics/perform_renderer.h"
#include "graphics/render_instance.h"
#include "voxel_renderer.h"

namespace restir_pt {
    using projv::core::vec2;
    using projv::core::vec3;
    using projv::core::vec4;

    constexpr float VIGNETTE = 0.15f;       // the corners' falloff: subtle
    constexpr float DOF_PIXELS = 1.5f;      // the far distance's blur radius at 1080 lines: very slight
    constexpr float BLOOM = 0.04f;          // the bloom's share of the image: a glow, not a haze
    constexpr float SHUTTER = 0.5f;         // the motion blur: the share of a frame's motion blurred over (film's 180 degrees)
    constexpr float BLUR_PIXELS = 12.0f;    // ...and its longest streak at 1080 lines: very subtle
    constexpr float SSAO = 0.85f;           // how far the obscurance darkens the indirect light
    constexpr float SUN_HALF_ANGLE = 0.02f; // radians, ~4x the real sun's: penumbrae a little soft
                                            // (pjv_path_light.sc's PATH_SUN_HALF_ANGLE: the same disc)
    // The light shafts (shafts.frag): the haze's optical depth over its falloff, the falloff's length in the
    // finest voxels (the air further out than about that is not counted: open sunlit air reads as a light
    // haze, not glare), the phase function's forward scattering, and the frames they are accumulated over.
    constexpr float SHAFT_STRENGTH = 0.2f;
    constexpr float SHAFT_FALLOFF_VOXELS = 48.0f;
    constexpr float SHAFT_PHASE_G = 0.6f;
    constexpr float SHAFT_HISTORY = 4.0f;   // short: ao_apply's radial blur takes the noise out
    // The aerial perspective: a thin haze over the whole scene, on top of the ground fog -- what makes the far
    // distance read as far. It thickens with distance (restirFog: its density grows from nothing at the camera
    // to AIR_DEPTH e-folds per fog length at one fog length out), so what is near stays crisp and only the
    // distance fades; a plain exponential that is visible on a mountain range already greys the trees under
    // the camera. Also the least height it thins over, as a share of the fog's length (a scene taller than that
    // thins over its own height, so a mountain range stands in the haze rather than above it). The light shafts'
    // second source is this same air, so shafts reach across a valley and not only a few dozen voxels.
    constexpr float AIR_DEPTH = 2.0f;
    constexpr float AIR_HEIGHT = 0.5f;
    constexpr float AIR_SHAFT_STRENGTH = 0.12f;
    // The normals' blur (normal_blur.frag): its width in voxel edges -- how far from an edge the shading
    // turns from one face to the next; outside edges only, and only half way (normal_blur.frag's
    // NORMAL_BLUR_STRENGTH). A tenth of an edge: a thin bevel that catches the light, the faces left flat.
    constexpr float NORMAL_BLUR = 0.1f;
    constexpr int   VIEWS = 7;
    // Rays a GI pixel may trace in the sample pass (a new path: 4; validating the history's suffix: 3;
    // shifting it here: 4 if it must be replayed) and in the spatial pass (its own path and five neighbours',
    // replayed only where a reconnection was too close to make, and a visibility ray). The paths see every
    // surface as rough (restir_common.sc), so nearly every shift is a reconnection and traces nothing.
    constexpr float SAMPLE_RAYS = 12.0f;
    constexpr float SPATIAL_RAYS = 8.0f;
    constexpr float VALIDATION = 0.1f;      // a path's light changed by more than this share: stale
    constexpr int   PATH_BOUNCES = 2;       // cheap; a third bounce cost half again for little light
    constexpr float RENDER_SCALE = 0.5f;    // its G-buffer, each way; resources.json's 0.5 targets

    // What the gallery's keys and switches turn; the defaults are the final render.
    struct Settings {
        int   view = 0;             // what it shows: the image, the indirect light, the direct, the AO, ... (VIEWS)
        bool  reuse = true;         // ReSTIR's reuse; off, each GI pixel's one fresh path, denoised
        bool  temporal = true, spatial = true, visibility = true;   // the reuse's halves, and its visibility test
        float atrous = 1.0f;        // the a-trous denoise
        bool  raw = false;          // no GI accumulation and no image history: each frame's estimate alone
        bool  bypass = false;       // the reconstruction bypassed: the render resolution point-sampled
        bool  halton16 = true;      // the TAA jitter over 16 Halton phases (false: the four window-pixel centres)
        struct Post { bool on = true, vignette = true, grade = true, exposure = true, dof = true, bloom = true, motionBlur = true; } post;
        bool  fog = true, shafts = true, softShadow = true, air = true, farAO = true;
        float ssao = SSAO;          // 0: no SSAO
        float normalBlur = NORMAL_BLUR;   // 0: the voxels' own hard normals
        float validation = 0.0f;    // the paths' validation (VALIDATION): off, it bought nothing measurable
        float bounces = float(PATH_BOUNCES);
        int   stride = 4;           // GI for one window pixel in stride x stride (4 or 2)
    };

    // The scene's scale: the finest voxel (what surfaces are offset by), and its size, lowest point and
    // height (the fog's and the haze's). fogLength 0: from the radius (below); a scene that streams
    // without end (the terrain) names its own.
    struct SceneScale {
        float finest = 1.0f, radius = 100.0f, ground = 0.0f, height = 0.0f, fogLength = 0.0f;
    };

    struct Frame {
        vec3  position{0.0f}, direction{0.0f, 0.0f, 1.0f};
        vec3  prevPosition{0.0f}, prevDirection{0.0f, 0.0f, 1.0f};
        vec3  sunDirection{0.0f, 1.0f, 0.0f};   // normalized
        bool  moved = false;      // the camera or the sun
        bool  sunMoved = false;   // the sun: the light changed, so every history starts over
        bool  first = false;      // the first frame drawn
        int   count = 0;          // frames so far
        vec2  window{1.0f};
        float seconds = 0.0f;     // since the last frame, for the exposure's adaptation
    };

    // Loads it as renderer `slot` and makes it the active one; false (logged) when it is not staged beside
    // the binary. Run from the binary's directory: its folders are named relative to it.
    inline bool install(projv::graphics::RenderInstance& renderInstance, int slot = 1) {
        if (!std::filesystem::exists("renderers/restir-pt/render.json")) {
            projv::core::error("restir-pt: no renderers/restir-pt beside the binary (the build stages it there)");
            return false;
        }
        renderInstance.addRendererSpecification(slot, projv::graphics::loadRendererSpecification("./renderers/restir-pt/"));
        bgfx::ShaderHandle vs = projv::graphics::loadShader("./renderers/restir-pt/shaders/vs_quad.bin");
        renderInstance.setActiveRenderer(projv::graphics::constructRendererSpecification(renderInstance.getRendererSpecification(slot), vs));
        return true;
    }

    // Every uniform its resources.json declares. `world`, given, uploads last frame's pose of every chunk for
    // the G-buffer's motion vectors (resources.json: prevPoses, texture 99).
    inline void upload(std::shared_ptr<projv::ConstructedRenderer> renderer, const Frame& frame, const SceneScale& scene,
                       const Settings& s, projv::World* world) {
        using projv::graphics::setUniformToValue;
        // Surfaces are offset a twentieth of the finest voxel before a ray leaves them; the same-surface
        // tests are scaled from it too.
        const float finest = scene.finest;
        // Which render texel of its block each GI pixel samples this frame (the block: the GI stride against
        // the render resolution, 2 or 1): every one in turn, diagonals first.
        const int giStride = std::max(1, int(float(s.stride) * RENDER_SCALE));
        static const int order4[4][2] = {{0, 0}, {1, 1}, {1, 0}, {0, 1}};
        const int* offset = order4[giStride == 2 ? frame.count % 4 : 0];
        // TAA: each render texel is traced through a different point of itself each frame -- a 16-phase Halton
        // sequence over the whole texel, so over the 2x2 window pixels it covers and within each. The resolve's
        // history then averages them into a supersampled image, and a voxel thinner than a texel is found some
        // of the time rather than always or never (the distance's aliasing). The reconstruction weighs each
        // texel's colour from where its ray actually went.
        auto halton = [](int index, int base) {
            float f = 1.0f, r = 0.0f;
            for (; index > 0; index /= base) { f /= float(base); r += f * float(index % base); }
            return r;
        };
        const int phase = frame.count % 16 + 1;
        vec4 jitterParams{halton(phase, 2) - 0.5f, halton(phase, 3) - 0.5f, 0.0f, 0.0f};
        if (!s.halton16) {
            static const int cellOrder[4] = {0, 3, 1, 2};
            const int cell = cellOrder[frame.count % 4];
            jitterParams = vec4((float(cell % 2) + 0.5f) * 0.5f - 0.5f, (float(cell / 2) + 0.5f) * 0.5f - 0.5f, 0.0f, 0.0f);
        }
        vec4 renderParams{0.0f, 0.0f, s.bypass ? 1.0f : 0.0f, 0.0f};
        // SSAO six of the finest voxels round, and how far it darkens the indirect light; the sun's penumbra per
        // unit of blocker distance (the disc's full width); the fog's length, 1.6 scene radii held to 300..3000
        // units (30..300 m at 10 units a metre), unless the scene names one.
        const float fogLength = scene.fogLength > 0.0f ? scene.fogLength : std::clamp(1.6f * scene.radius, 300.0f, 3000.0f);
        // The SSAO and the fog are the final render's, not the path tracer's.
        const bool finalRender = s.post.on;
        vec4 look{6.0f * finest, finalRender ? s.ssao : 0.0f, s.softShadow ? 2.0f * std::tan(SUN_HALF_ANGLE) : 0.0f, s.farAO ? 1.0f : 0.0f};
        // The height fog: at the ground, 0.6 of an e-fold over the fog's length (about 45% hidden there,
        // gathering from nothing), thinning over 0.15 of it in height, never more than 90%.
        vec4 haze{finalRender && s.fog ? 0.6f / fogLength : 0.0f, scene.ground, 0.15f * fogLength, 0.9f};
        setUniformToValue(renderer, "restirHaze", haze);
        // The aerial haze: AIR_DEPTH e-folds per fog length at the ground once a fog length out, thinning over
        // the scene's height (at least half the fog's length); w the length its onset grows over.
        const float airHeight = std::max(scene.height, AIR_HEIGHT * fogLength);
        const float airDensity = finalRender && s.fog && s.air ? AIR_DEPTH / fogLength : 0.0f;
        vec4 air{airDensity, scene.ground, airHeight, fogLength};
        setUniformToValue(renderer, "restirAir", air);
        vec4 shafts{finalRender && s.shafts ? SHAFT_STRENGTH : 0.0f, 1.0f / (SHAFT_FALLOFF_VOXELS * finest), SHAFT_PHASE_G, SHAFT_HISTORY};
        // The shafts' second source, the aerial haze: its density at the camera's height (constant along the ray:
        // the shafts are soft, and the camera is where they are seen from), so they reach as far as the haze does.
        const float airAtCamera = airDensity * std::exp(-std::max(frame.position.y - scene.ground, 0.0f) / airHeight);
        vec4 shaftsAir{AIR_SHAFT_STRENGTH, airAtCamera, fogLength, 0.0f};
        setUniformToValue(renderer, "restirShaftsAir", shaftsAir);
        // y: the clock the water's waves move by (seconds, wrapped every hour so a float keeps its precision).
        static float clock = 0.0f;
        clock = std::fmod(clock + frame.seconds, 3600.0f);
        vec4 normals{s.normalBlur, clock, 0.0f, 0.0f};
        setUniformToValue(renderer, "restirNormals", normals);

        vec4 cameraPos{frame.position, 1.0f}, cameraDir{frame.direction, 0.0f};
        vec4 prevCameraPos{frame.prevPosition, 1.0f}, prevCameraDir{frame.prevDirection, 0.0f};
        vec4 sunDir{frame.sunDirection, 0.0f};
        // z: the sun moved -- the light changed, so every history (the reservoirs' cached light included) starts over.
        vec4 frameCount{float(frame.count), frame.moved ? 1.0f : 0.0f, frame.sunMoved ? 1.0f : 0.0f, 0.0f};
        vec4 reuse{s.reuse && s.temporal ? 1.0f : 0.0f, s.reuse && s.spatial ? 1.0f : 0.0f, s.visibility ? 1.0f : 0.0f, 4.0f};   // temporal, spatial, visibility, M cap
        vec4 denoise{s.atrous, s.raw ? 0.0f : 1.0f, float(s.view), 12.0f};   // a-trous, GI accumulation, view, its cap
        vec4 jitter{float(offset[0]), float(offset[1]), float(giStride), frame.first ? 1.0f : 0.0f};
        // w: one render texel's width per unit of distance, for the tests that ask "is this the same surface?".
        const float renderHeight = std::max(frame.window.y * RENDER_SCALE, 1.0f);
        const float pixelWidth = 2.0f * std::tan(30.0f * 3.14159265f / 180.0f) / renderHeight;
        // The image's history: none when the sun moved (the light is new); short while the camera moves, so
        // changing light is not dragged along; 64 frames on a still view -- enough to average the jitter and
        // what noise is left, short enough that the GI's own history, not this one, decides how fast it settles.
        const float imageHistory = frame.sunMoved || s.raw ? 1.0f : frame.moved ? 16.0f : 64.0f;
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
        vec4 path{s.bounces, SAMPLE_RAYS, SPATIAL_RAYS, s.validation};
        setUniformToValue(renderer, "restirPath", path);
        const Settings::Post& post = s.post;
        vec4 postParams{post.on && post.vignette ? VIGNETTE : 0.0f, post.on && post.grade ? 1.0f : 0.0f,
                        post.on && post.exposure ? 1.0f : 0.0f, frame.seconds};
        setUniformToValue(renderer, "restirPost", postParams);
        vec4 dof{post.on && post.dof ? DOF_PIXELS * frame.window.y / 1080.0f : 0.0f,
                 post.on && post.bloom ? BLOOM : 0.0f,                // y: display.frag's bloom
                 post.on && post.motionBlur ? SHUTTER : 0.0f,         // zw: motion_blur.frag's
                 BLUR_PIXELS * frame.window.y / 1080.0f};
        setUniformToValue(renderer, "restirDof", dof);
        if (world) voxel_renderer::uploadPoses(*world, renderer, 99);
        setUniformToValue(renderer, "restirShafts", shafts);
        setUniformToValue(renderer, "jitterParams", jitterParams);
        setUniformToValue(renderer, "renderParams", renderParams);
    }
}
