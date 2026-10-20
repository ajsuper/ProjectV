// The renderer: Voxel Mayhem's ten passes (ProjectV examples/shared/voxelRenderer), turned into a
// moonlit night, with the explosion as a second, shadowed light and one pass more -- smoke.frag, which
// marches the mushroom cloud's density grid and display.frag composites over the frame.
//
//     night_renderer::install(world, renderInstance, here);   // at startup
//     night_renderer::uploadSmoke(world, renderInstance, atlas);   // when the grid changes
//     night_renderer::upload(world, renderInstance, eye, forward);  // each frame, then render
//
// Its look is the Look in world.ctx().
#pragma once

#include <cmath>
#include <filesystem>
#include <vector>

#include <bx/uint32_t.h>

#include "core/math.h"
#include "core/world.h"
#include "graphics/disk_io.h"
#include "graphics/gpu_interface.h"
#include "graphics/manage_resources.h"
#include "graphics/perform_renderer.h"
#include "graphics/render_instance.h"

namespace night_renderer {
    using projv::core::vec3;
    using projv::core::vec4;

    // The smoke grid: GRID cells a side, uploaded as an atlas of its z slices, TILES to a row.
    constexpr int GRID = 96, TILES = 10, ATLAS = GRID * TILES;
    // The embers' grid: its own, coarser (a lump is small, and a cell holds many), over the same box.
    constexpr int EMBER_GRID = 84;
    constexpr float EMBER_CELL = 4.0f;
    constexpr uint32_t SMOKE_TEXTURE = 17;   // resources.json's smokeGrid
    // The embers' lumps (see gbuffer.frag): the lumps, four texels each; a (first, count) per grid cell;
    // and the cube numbers cell by cell. Sizes as resources.json declares them.
    constexpr uint32_t CUBES_TEXTURE = 19, CELLS_TEXTURE = 20, INDEX_TEXTURE = 21;
    constexpr int DATA_WIDE = 1024, CUBE_ROWS = 1024, CELL_ROWS = 640, INDEX_ROWS = 2048;
    constexpr int MAX_CUBES = DATA_WIDE * CUBE_ROWS / 4, MAX_ENTRIES = DATA_WIDE * INDEX_ROWS;
    static_assert(EMBER_GRID * EMBER_GRID * EMBER_GRID <= DATA_WIDE * CELL_ROWS, "the ember grid's cells must fit emberCells");

    // One ember lump (a turned 3x3x3 of voxels, some missing): where, half its edge, its turn, its colour,
    // its glow (a multiple of the colour), and the seed that picks which voxels are missing.
    struct Cube { vec3 centre; float half; projv::core::quat rotation; vec3 colour; float glow; float seed; };

    struct Look {
        bool  shadows = true, ambientOcclusion = true, taa = true, rayGI = true;
        // The smoke pass: 0 none (the cloud is its voxel chunks alone), 1 the grid drawn as solid
        // blocks, 2 the grid marched as soft gas. Debug views: 8 the grid's box, 9 its density.
        int   smokeMode = 0;
        float blockThreshold = 0.07f;   // blocks: a cell this dense or more is solid
        float blockSubdivision = 2.0f;  // blocks: lattice cells to a grid cell, each way
        int   bounce = 1;
        int   debugView = 0;
        float renderScale = 0.5f;
        float finestVoxel = 0.125f;
        float hazeRadius = 500.0f;
        // Day or night, and the light of each: toward the sun or the moon, how bright it is, how bright the
        // sky. Day is Voxel Mayhem's. Night is dark -- a thin moon on a moonless-dark sky -- so the city is
        // its windows and street lamps, and the fireball, when it comes, lights everything. The exposure
        // starts at the time of day's (dayExposure, nightExposure).
        bool  day = true;
        vec3  sun = glm::normalize(vec3(-0.417f, 0.758f, 0.500f));
        vec3  moon = glm::normalize(vec3(0.62f, 0.52f, -0.58f));
        float sunLight = 8.0f, daySky = 1.0f, moonLight = 0.07f, nightSky = 0.08f;
        static constexpr float dayExposure = 1.0f, nightExposure = 1.25f;
        float exposure = dayExposure;
        // The explosion's light: where, how bright (0: none), its colour and the radius it softens over.
        vec3  flashAt{0.0f};
        float flash = 0.0f;
        vec3  flashColour{1.0f, 0.55f, 0.2f};
        float flashRadius = 4.0f;
        // The smoke grid's corner and cell, and how the gas looks.
        vec3  smokeMin{0.0f};
        float smokeCell = 3.5f;
        float smokeExtinction = 2.6f, fireEmission = 12.0f;
        float noiseErosion = 1.0f, noiseFrequency = 0.2f, noiseRise = 1.2f, smokeSteps = 72.0f;
        float seconds = 0.0f;
        // Last frame's camera, for TAA and the GI history.
        vec3  prevPosition{0.0f}, prevDirection{0.0f, 0.0f, 1.0f};
        bool  hasPrevious = false;
        int   frame = 0;
        // Ours, not the engine's: sampled textures rather than render targets, so they can be updated.
        bgfx::TextureHandle smokeTexture = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle cubesTexture = BGFX_INVALID_HANDLE, cellsTexture = BGFX_INVALID_HANDLE,
                            indexTexture = BGFX_INVALID_HANDLE;
        bool embers = true;
        int  cubeCount = 0;
    };

    // Uploads the grid: `atlas` is ATLAS x ATLAS texels of four floats (density, heat, dust, vapour),
    // converted to half floats on the way.
    inline void uploadSmoke(projv::World& world, const std::vector<float>& atlas) {
        Look& look = world.ctx().get<Look>();
        if (!bgfx::isValid(look.smokeTexture)) return;
        const uint32_t texels = uint32_t(ATLAS) * ATLAS;
        const bgfx::Memory* memory = bgfx::alloc(texels * 4 * sizeof(uint16_t));
        auto* out = reinterpret_cast<uint16_t*>(memory->data);
        for (size_t i = 0; i < size_t(texels) * 4; i++) out[i] = bx::halfFromFloat(atlas[i]);
        bgfx::updateTexture2D(look.smokeTexture, 0, 0, 0, 0, ATLAS, ATLAS, memory);
    }

    // Bins `cubes` into the ember grid (the smoke grid's box and cells) and uploads all three textures.
    // Sparse: only the cells some cube touches are counted, written and cleared again next frame, and only
    // the rows in use go up -- the grid is 84^3 cells, a cloud touches a few thousand of them, and walking
    // and zero-filling all of them (and the full-size buffers) every frame was most of the cost.
    // Scratch vectors are kept between frames. Returns how many cubes went up.
    inline int uploadEmbers(projv::World& world, const std::vector<Cube>& cubes) {
        Look& look = world.ctx().get<Look>();
        constexpr int G = EMBER_GRID;
        static std::vector<float> cubeData(size_t(DATA_WIDE) * CUBE_ROWS * 4, 0.0f), cellData(size_t(DATA_WIDE) * CELL_ROWS * 2, 0.0f),
            indexData(size_t(DATA_WIDE) * INDEX_ROWS, 0.0f);
        static std::vector<uint32_t> counts(size_t(G) * G * G, 0u), cursor(size_t(G) * G * G, 0u);
        static std::vector<uint32_t> touched, lastTouched;
        static int lastLowRow = 0, lastHighRow = -1;
        // Last frame's cells, cleared.
        for (uint32_t c : lastTouched) cellData[size_t(c) * 2] = cellData[size_t(c) * 2 + 1] = 0.0f;
        touched.clear();
        const int n = std::min(int(cubes.size()), MAX_CUBES);
        // Each cube goes in every cell its bounding sphere touches.
        auto range = [&](const Cube& c, projv::core::ivec3& lo, projv::core::ivec3& hi) {
            const float reach = c.half * 1.7321f;
            const vec3 a = (c.centre - reach - look.smokeMin) / EMBER_CELL, b = (c.centre + reach - look.smokeMin) / EMBER_CELL;
            lo = glm::clamp(projv::core::ivec3(glm::floor(a)), projv::core::ivec3(0), projv::core::ivec3(G - 1));
            hi = glm::clamp(projv::core::ivec3(glm::floor(b)), projv::core::ivec3(0), projv::core::ivec3(G - 1));
            return glm::all(glm::greaterThanEqual(b, vec3(0.0f))) && glm::all(glm::lessThan(a, vec3(float(G))));
        };
        for (int i = 0; i < n; i++) {
            projv::core::ivec3 lo, hi;
            if (!range(cubes[size_t(i)], lo, hi)) continue;
            for (int z = lo.z; z <= hi.z; z++)
                for (int y = lo.y; y <= hi.y; y++)
                    for (int x = lo.x; x <= hi.x; x++)
                        if (counts[size_t((z * G + y) * G + x)]++ == 0) touched.push_back(uint32_t((z * G + y) * G + x));
        }
        uint32_t total = 0, lowCell = ~0u, highCell = 0;
        for (uint32_t c : touched) {
            const uint32_t room = std::min<uint32_t>(counts[c], uint32_t(MAX_ENTRIES) - std::min<uint32_t>(total, MAX_ENTRIES));
            cellData[size_t(c) * 2 + 0] = float(total);
            cellData[size_t(c) * 2 + 1] = float(room);
            cursor[c] = total;
            total += room;
            lowCell = std::min(lowCell, c);
            highCell = std::max(highCell, c);
        }
        for (int i = 0; i < n; i++) {
            const Cube& c = cubes[size_t(i)];
            float* out = &cubeData[size_t(i) * 16];
            out[0] = c.centre.x; out[1] = c.centre.y; out[2] = c.centre.z; out[3] = c.half;
            out[4] = c.rotation.x; out[5] = c.rotation.y; out[6] = c.rotation.z; out[7] = c.rotation.w;
            out[8] = c.colour.r; out[9] = c.colour.g; out[10] = c.colour.b; out[11] = c.glow;
            out[12] = c.seed;
            projv::core::ivec3 lo, hi;
            if (!range(c, lo, hi)) continue;
            for (int z = lo.z; z <= hi.z; z++)
                for (int y = lo.y; y <= hi.y; y++)
                    for (int x = lo.x; x <= hi.x; x++) {
                        const size_t cell = size_t((z * G + y) * G + x);
                        const uint32_t end = uint32_t(cellData[cell * 2]) + uint32_t(cellData[cell * 2 + 1]);
                        if (cursor[cell] < end) indexData[cursor[cell]++] = float(i);
                    }
        }
        for (uint32_t c : touched) counts[c] = 0u;
        // The cell rows: those touched this frame or last (cleared since), in one span.
        const int cellsWide = DATA_WIDE;   // cells a row (two floats each, RG)
        int lowRow = touched.empty() ? 0 : int(lowCell) / cellsWide, highRow = touched.empty() ? -1 : int(highCell) / cellsWide;
        int fromRow = std::min(lowRow, lastHighRow >= 0 ? lastLowRow : lowRow);
        int toRow = std::max(highRow, lastHighRow);
        static bool whole = true;   // the first upload: all of it, zeros (the texture is made without data)
        if (whole) fromRow = 0, toRow = CELL_ROWS - 1, whole = false;
        if (toRow >= fromRow)
            bgfx::updateTexture2D(look.cellsTexture, 0, 0, 0, uint16_t(fromRow), DATA_WIDE, uint16_t(toRow - fromRow + 1),
                                  bgfx::copy(&cellData[size_t(fromRow) * DATA_WIDE * 2], uint32_t(toRow - fromRow + 1) * DATA_WIDE * 8));
        lastLowRow = lowRow, lastHighRow = highRow;
        std::swap(touched, lastTouched);
        // Only the rows in use, for the cubes and the index.
        const int cubeRows = std::max(1, (n * 4 + DATA_WIDE - 1) / DATA_WIDE);
        const int indexRows = std::max(1, int((total + DATA_WIDE - 1) / DATA_WIDE));
        bgfx::updateTexture2D(look.cubesTexture, 0, 0, 0, 0, DATA_WIDE, uint16_t(cubeRows),
                              bgfx::copy(cubeData.data(), uint32_t(cubeRows) * DATA_WIDE * 16));
        bgfx::updateTexture2D(look.indexTexture, 0, 0, 0, 0, DATA_WIDE, uint16_t(indexRows),
                              bgfx::copy(indexData.data(), uint32_t(indexRows) * DATA_WIDE * 4));
        look.cubeCount = n;
        return n;
    }

    inline void install(projv::World& world, projv::graphics::RenderInstance& renderInstance, const std::filesystem::path& here) {
        Look& look = world.ctx().contains<Look>() ? world.ctx().get<Look>() : world.ctx().emplace<Look>();
        projv::RendererSpecification specification =
            projv::graphics::loadRendererSpecification((here / "nightRenderer").string() + "/");
        for (projv::Texture& texture : specification.resources.textures)
            if (texture.sizeMode == projv::TextureSizeMode::Relative && texture.name != "reconstructed" && texture.name != "taaColor")
                texture.scale *= look.renderScale;
        renderInstance.addRendererSpecification(1, specification);
        bgfx::ShaderHandle vertexShader = projv::graphics::loadShader((here / "nightRenderer/shaders/vs_quad.bin").string());
        renderInstance.setActiveRenderer(
            projv::graphics::constructRendererSpecification(renderInstance.getRendererSpecification(1), vertexShader));

        // The engine makes every texture in resources.json a render target, and a render target cannot
        // be updated from the CPU: the smoke grid's handle is swapped for a plain sampled texture.
        // Nothing renders into it, so no framebuffer holds the old one.
        auto renderer = renderInstance.getActiveRenderer();
        auto& handles = renderer->resources.textures.textureHandles;
        if (bgfx::isValid(handles.at(SMOKE_TEXTURE))) bgfx::destroy(handles.at(SMOKE_TEXTURE));
        // Made without data: bgfx makes a texture given its contents at creation immutable, and every
        // update to it afterwards is dropped.
        look.smokeTexture = bgfx::createTexture2D(ATLAS, ATLAS, false, 1, bgfx::TextureFormat::RGBA16F,
                                                  BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        std::vector<float> empty(size_t(ATLAS) * ATLAS * 4, 0.0f);
        uploadSmoke(world, empty);
        // The embers' three, likewise.
        auto swap = [&](uint32_t id, int rows, bgfx::TextureFormat::Enum format) {
            if (bgfx::isValid(handles.at(id))) bgfx::destroy(handles.at(id));
            handles[id] = bgfx::createTexture2D(DATA_WIDE, uint16_t(rows), false, 1, format,
                                                BGFX_SAMPLER_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
            return handles[id];
        };
        look.cubesTexture = swap(CUBES_TEXTURE, CUBE_ROWS, bgfx::TextureFormat::RGBA32F);
        look.cellsTexture = swap(CELLS_TEXTURE, CELL_ROWS, bgfx::TextureFormat::RG32F);
        look.indexTexture = swap(INDEX_TEXTURE, INDEX_ROWS, bgfx::TextureFormat::R32F);
        uploadEmbers(world, {});
        handles[SMOKE_TEXTURE] = look.smokeTexture;
    }

    // Sets this frame's uniforms from the Look and the camera.
    inline void upload(projv::World& world, projv::graphics::RenderInstance& renderInstance, vec3 position, vec3 direction) {
        using projv::graphics::setUniformToValue;
        auto renderer = renderInstance.getActiveRenderer();
        Look& look = world.ctx().get<Look>();
        if (!look.hasPrevious) {
            look.prevPosition = position;
            look.prevDirection = direction;
            look.hasPrevious = true;
        }
        const bool moved = glm::length(position - look.prevPosition) > 1e-5f || glm::length(direction - look.prevDirection) > 1e-6f;
        vec3 prevPosition = look.prevPosition, prevDirection = look.prevDirection;
        setUniformToValue(renderer, "cameraPos", position);
        setUniformToValue(renderer, "cameraDir", direction);
        setUniformToValue(renderer, "prevCameraPos", prevPosition);
        setUniformToValue(renderer, "prevCameraDir", prevDirection);
        vec4 key(look.day ? look.sun : look.moon, 0.0f);
        setUniformToValue(renderer, "sunDir", key);
        vec4 light(look.day ? look.sunLight : look.moonLight, look.day ? look.daySky : look.nightSky, look.exposure, look.finestVoxel);
        setUniformToValue(renderer, "lightParams", light);
        vec4 fog(-std::log(0.92f) / look.hazeRadius, look.hazeRadius * 0.35f, 0.0f, 0.25f);
        setUniformToValue(renderer, "fogParams", fog);
        vec4 frameParams(float(look.frame++), 0.0f, look.taa ? 1.0f : 0.0f, moved ? 1.0f : 0.0f);
        setUniformToValue(renderer, "frameParams", frameParams);
        vec4 renderParams(look.shadows ? 1.0f : 0.0f, look.ambientOcclusion ? 1.0f : 0.0f, 0.0f, float(look.debugView));
        setUniformToValue(renderer, "renderParams", renderParams);
        static const float bounceStrength[3] = {0.0f, 2.5f, 5.0f};
        vec4 giParams(look.ambientOcclusion ? bounceStrength[look.bounce] : 0.0f, look.rayGI ? 1.0f : 0.0f, 0.0f, 0.0f);
        setUniformToValue(renderer, "giParams", giParams);
        vec4 flashParams(look.flashAt, look.flash);
        setUniformToValue(renderer, "flashParams", flashParams);
        vec4 flashColor(look.flashColour, look.flashRadius);
        setUniformToValue(renderer, "flashColor", flashColor);
        vec4 skyParams(look.day ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
        setUniformToValue(renderer, "skyParams", skyParams);
        vec4 smokeBox(look.smokeMin, look.smokeCell);
        setUniformToValue(renderer, "smokeBox", smokeBox);
        vec4 smokeGridInfo{float(GRID), float(TILES), float(ATLAS), float(ATLAS)};
        setUniformToValue(renderer, "smokeGridInfo", smokeGridInfo);
        vec4 smokeParams(look.seconds, look.smokeExtinction, float(look.smokeMode), look.fireEmission);
        setUniformToValue(renderer, "smokeParams", smokeParams);
        vec4 smokeShape(look.noiseErosion, look.noiseFrequency, look.noiseRise, look.smokeSteps);
        setUniformToValue(renderer, "smokeShape", smokeShape);
        vec4 smokeBlocks(look.blockThreshold, look.blockSubdivision, 0.0f, 0.0f);
        setUniformToValue(renderer, "smokeBlocks", smokeBlocks);
        vec4 emberGrid(look.smokeMin, EMBER_CELL);
        setUniformToValue(renderer, "emberGrid", emberGrid);
        vec4 emberInfo{float(EMBER_GRID), float(DATA_WIDE), float(DATA_WIDE), look.embers && look.cubeCount > 0 ? 1.0f : 0.0f};
        setUniformToValue(renderer, "emberInfo", emberInfo);
        look.prevPosition = position;
        look.prevDirection = direction;
    }
}
