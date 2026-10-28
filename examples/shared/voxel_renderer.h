// The examples' shared renderer (examples/shared/voxelRenderer/): a G-buffer (one primary and one sun
// shadow ray a render pixel), GI from a grid of probes round the camera, AO at two scales, the shade, the
// voxel reconstruction, TAA and the display.
// First written for Voxel Mayhem (examples/80-voxel-mayhem-demo); plan: docs/plans/voxel-renderer.md.
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
#include <vector>

#include "core/math.h"
#include "core/world.h"
#include "graphics/disk_io.h"
#include "graphics/engine_shaders.h"
#include "graphics/gpu_interface.h"
#include "graphics/manage_resources.h"
#include "graphics/perform_renderer.h"
#include "graphics/render_instance.h"

namespace voxel_renderer {
    using projv::core::vec3;
    using projv::core::vec4;

    struct View {
        bool  shadows = true, ambientOcclusion = true, taa = true;
        bool  gi = true;           // the probe grid's light; off: the sky ambient alone
        float giSpacing = 0.0f;    // between probes, world units; 0: sixteen of the finest voxels
        int   debugView = 0;       // 0 off, 1 reconstruction coverage, 2 AO only, 3 sun visibility, 4 motion, 7 GI
        float sun = 0.86f;         // the sun's angle up its arc
        bool  crosshair = false;   // two bars through the centre, for aiming
        float jitter = 1.0f;       // the TAA's sub-texel jitter, in texels across
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

    // Every chunk's pose last frame, for the G-buffer's motion vectors: what `upload` sends, and this
    // frame's, which it keeps for the next. Per header slot (the chunk's index), with what was in the slot
    // -- slots are reused, and a new chunk in an old slot did not move from the old one's pose.
    struct Poses {
        static constexpr uint32_t TEXTURE = 11;            // resources.json: prevPoses
        static constexpr uint32_t WIDTH = 1024, HEIGHT = 512;   // texels; two a slot
        std::vector<float> then, now;                       // 8 floats a slot: position, known; rotation
        std::vector<uint64_t> thenWho, nowWho;
    };

    inline uint64_t poseOwner(const projv::Chunk& chunk) {
        return (uint64_t(chunk.header.chunkID) << 32) | uint64_t(uint32_t(chunk.componentHandle));
    }

    // Sends last frame's poses and keeps this frame's. A chunk new to its slot is sent where it is now.
    // `texture`: the renderer's prevPoses texture ID (another renderer may use a different slot).
    inline void uploadPoses(projv::World& world, std::shared_ptr<projv::ConstructedRenderer> renderer,
                            uint32_t texture = Poses::TEXTURE) {
        const projv::Scene* scene = world.ctx().find<projv::Scene>();
        auto handle = renderer->resources.textures.textureHandles.find(texture);
        if (!scene || handle == renderer->resources.textures.textureHandles.end() || !bgfx::isValid(handle->second)) return;
        Poses& poses = world.ctx().contains<Poses>() ? world.ctx().get<Poses>() : world.ctx().emplace<Poses>();
        const size_t slots = std::min<size_t>(scene->chunks.size(), size_t(Poses::WIDTH / 2) * Poses::HEIGHT);
        poses.now.assign(slots * 8, 0.0f);
        poses.nowWho.assign(slots, 0);
        for (size_t h = 0; h < slots; h++) {
            const projv::Chunk& chunk = scene->chunks[h];
            if (!chunk.alive) continue;
            const projv::core::vec3 p = chunk.header.position;
            const projv::core::quat q = chunk.header.rotation;
            float* out = &poses.now[h * 8];
            out[0] = p.x; out[1] = p.y; out[2] = p.z; out[3] = 1.0f;
            out[4] = q.x; out[5] = q.y; out[6] = q.z; out[7] = q.w;
            poses.nowWho[h] = poseOwner(chunk);
        }
        // What the G-buffer reads as last frame: last frame's pose where the slot held the same chunk.
        std::vector<float> sent = poses.now;
        for (size_t h = 0; h < slots && h < poses.thenWho.size(); h++)
            if (poses.thenWho[h] == poses.nowWho[h] && poses.then[h * 8 + 3] > 0.5f)
                std::copy(&poses.then[h * 8], &poses.then[h * 8] + 8, &sent[h * 8]);
        const uint32_t perRow = Poses::WIDTH / 2;
        const uint32_t rows = uint32_t((slots + perRow - 1) / perRow);
        if (rows > 0) {
            sent.resize(size_t(rows) * perRow * 8, 0.0f);
            bgfx::updateTexture2D(handle->second, 0, 0, 0, 0, uint16_t(Poses::WIDTH), uint16_t(rows),
                                  bgfx::copy(sent.data(), uint32_t(sent.size() * sizeof(float))));
        }
        std::swap(poses.then, poses.now);
        std::swap(poses.thenWho, poses.nowWho);
    }

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
        bgfx::ShaderHandle vertexShader = projv::graphics::loadEngineShader("vs_quad");
        renderInstance.setActiveRenderer(
            projv::graphics::constructRendererSpecification(renderInstance.getRendererSpecification(1), vertexShader));
    }

    // The radical inverse of `index` in `base`: Halton's sequence, for the jitter.
    inline float halton(int index, int base) {
        float f = 1.0f, r = 0.0f;
        for (; index > 0; index /= base) {
            f /= float(base);
            r += f * float(index % base);
        }
        return r;
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
        // With TAA, each frame traces through the centre of another of the output pixels a render texel
        // covers (2x2 of them at half scale): over the cycle every output pixel is sampled exactly, and
        // a still view repeats the same samples, so it settles to the full-resolution image and stays.
        const int across = std::max(1, int(std::lround(1.0f / view.renderScale)));
        static const int ORDER[4] = {0, 3, 1, 2};   // diagonals first: each half of the cycle covers the texel
        const int cell = across == 2 ? ORDER[view.frame % 4] : view.frame % (across * across);
        const float cx = (float(cell % across) + 0.5f) / float(across) - 0.5f, cy = (float(cell / across) + 0.5f) / float(across) - 0.5f;
        vec4 jitter = view.taa ? vec4(cx, cy, 0.0f, 0.0f) * view.jitter : vec4(0.0f);
        setUniformToValue(renderer, "jitterParams", jitter);
        vec4 frameParams(float(view.frame++), 0.0f, view.taa ? 1.0f : 0.0f, moved ? 1.0f : 0.0f);
        setUniformToValue(renderer, "frameParams", frameParams);
        // The GI's probe grid (grr_gi.sc): GI_X x GI_Y x GI_Z cells round the camera, in whole cells.
        const float spacing = view.giSpacing > 0.0f ? view.giSpacing : 16.0f * view.finestVoxel;
        vec4 giParams(view.gi ? 1.0f : 0.0f, spacing, float(view.frame), 0.0f);
        setUniformToValue(renderer, "giParams", giParams);
        vec4 giOrigin(glm::floor(position / spacing) - vec3(16.0f, 8.0f, 16.0f), 0.0f);
        setUniformToValue(renderer, "giOrigin", giOrigin);
        vec4 renderParams(view.shadows ? 1.0f : 0.0f, view.ambientOcclusion ? 1.0f : 0.0f, 0.0f, float(view.debugView));
        setUniformToValue(renderer, "renderParams", renderParams);
        view.prevPosition = position;
        view.prevDirection = direction;
        uploadPoses(world, renderer);
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
