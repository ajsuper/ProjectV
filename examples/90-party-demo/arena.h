// The party's arenas and everything in them: five floating arenas over a lava sea -- one for each
// game -- made of tiles that drop away and plots that are built on and bombed; the players, the bombs,
// and Sky Dash's sweepers and pushers. Every model is made voxel by
// voxel here, each colour naming the physics material it is made of; the scene folder the server
// and every client load is written from this (`party_demo_server --write-assets <dir>`).
//
// A tile drops by taking away its anchor: each tile is an island of voxels in its floor, held up
// by one voxel of an anchor material (PhysicsMaterial::anchor) in the middle of its underside.
// Remove that voxel and the floor's support pass finds the tile standing on nothing, and it falls --
// a piece of its own, replicated to every client like any break.
#pragma once

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "data_structures/scene.h"
#include "utils/compose_io.h"
#include "utils/editing.h"
#include "utils/material.h"
#include "utils/scene_query.h"

namespace party {
    using projv::core::ivec3;
    using projv::core::quat;
    using projv::core::vec3;

    inline uint32_t rgb(float r, float g, float b) { return projv::packRGB10(r, g, b); }

    // Small changes of shade by position, so a surface reads as made of something.
    inline float shade(int x, int y, int z, float amount) {
        // Unsigned arithmetic: a signed multiply that overflows is undefined.
        uint32_t h = uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u ^ uint32_t(z) * 83492791u;
        return 1.0f - amount + amount * float(h % 4) / 3.0f;   // four levels: a palette holds 255
    }

    // ---- The games -----------------------------------------------------------------------------------

    enum Mode : uint8_t { Crumble = 0, Tag = 1, Dash = 2, Climb = 3, Blueprint = 4 };
    constexpr int MODES = 5;
    inline const char* modeName(Mode m) {
        static const char* names[MODES] = {"CRUMBLE", "BOMB TAG", "SKY DASH", "SKY CLIMB", "BLUEPRINT"};
        return names[m];
    }
    // Each game's arena stands apart from the others, along x. Sky Dash's course runs along z.
    constexpr float ARENA_X[MODES] = {-70.0f, 0.0f, 70.0f, -150.0f, 150.0f};
    inline vec3 arenaCentre(Mode m) { return vec3(ARENA_X[m], 0.0f, 0.0f); }

    constexpr int   MAX_PLAYERS = 4;
    constexpr float LAVA_Y = -30.0f;

    // ---- Tiles -----------------------------------------------------------------------------------------
    //
    // A tile is 1.5 m square, with a quarter-metre gap to the next: a pitch of 1.75 m, seven voxels
    // of a quarter metre. A floor is a grid of them, nx by nz from its corner, `thick` voxels deep, its
    // top at `top`; a round floor leaves out the cells outside `radius`. A cell can also be left empty
    // but buildable: a gap a player can lay a tile into.
    constexpr float VOXEL = 0.25f;
    constexpr int   TILE_VOXELS = 6, PITCH_VOXELS = 7;
    constexpr float PITCH = PITCH_VOXELS * VOXEL;

    enum Cell : uint8_t { None = 0, Solid, Gap };

    struct FloorSpec {
        const char* name;
        Mode        mode;
        float       x0, z0, top;   // m: its grid's corner (least x, least z), and its walking surface
        int         nx, nz;
        float       radius;        // m from the grid's middle: cells further out are left out; 0 none
        int         thick;         // voxels
        vec3        colour[2];     // a checker of two
        bool        fixed;         // its tiles never drop: start, checkpoints, finish
        bool        crumbles;      // its tiles crack under foot and drop
        Cell      (*cell)(int i, int j) = nullptr;   // what each cell is; nullptr: all solid
    };

    // A square grid of n by n centred on (x, z).
    inline float gridCorner(float centre, int n) { return centre - float(n) * PITCH * 0.5f; }

    // Sky Dash's course, start to finish along +z, seven tiles wide.
    constexpr float DASH_X = 70.0f, DASH_START = -46.0f;
    constexpr float DASH_FINISH = 33.5f;              // z: across this line is home
    constexpr float DASH_RING_Z = -25.0f + 4.5f * PITCH;   // the sweepers' ring's middle
    inline Cell dashGaps(int, int j) { return (j >= 2 && j < 6) || (j >= 8 && j < 12) ? Gap : Solid; }
    inline Cell dashNarrow(int i, int) { return i >= 2 && i <= 4 ? Solid : None; }
    // Where a fallen racer comes back: the highest of these it has passed.
    constexpr float DASH_CHECKPOINTS[3] = {-42.5f, -7.5f, 17.0f};

    // Every floor of every arena. The server and the asset writer both read this, so a tile is found
    // where the files put it.
    inline const std::vector<FloorSpec>& floorSpecs() {
        static const std::vector<FloorSpec> specs = [] {
            std::vector<FloorSpec> f;
            const vec3 crumble = arenaCentre(Crumble), tag = arenaCentre(Tag);
            // Crumble: two floors, eight metres apart. Fall through the top one and the bottom one is
            // your last chance.
            f.push_back({"crumble top", Crumble, gridCorner(crumble.x, 13), gridCorner(0.0f, 13), 8.0f, 13, 13, 11.6f, 2,
                         {vec3(0.25f, 0.72f, 0.78f), vec3(0.18f, 0.55f, 0.66f)}, false, true});
            f.push_back({"crumble bottom", Crumble, gridCorner(crumble.x, 13), gridCorner(0.0f, 13), 0.0f, 13, 13, 11.6f, 2,
                         {vec3(0.62f, 0.38f, 0.82f), vec3(0.46f, 0.28f, 0.66f)}, false, true});
            // Bomb tag: one wide floor.
            f.push_back({"tag floor", Tag, gridCorner(tag.x, 15), gridCorner(0.0f, 15), 0.0f, 15, 15, 13.2f, 2,
                         {vec3(0.95f, 0.62f, 0.2f), vec3(0.92f, 0.88f, 0.78f)}, false, false});
            // Sky Dash, piece by piece: the start, a strip that crumbles, the sweepers' ring, a
            // checkpoint, two gaps to bridge, a checkpoint, the narrow way past the pushers, home.
            const float dx = gridCorner(DASH_X, 7);
            const vec3 white(0.9f, 0.9f, 0.88f), grey(0.55f, 0.56f, 0.6f);
            f.push_back({"dash start", Dash, dx, -46.0f, 0.0f, 7, 4, 0.0f, 2, {white, grey}, true, false});
            f.push_back({"dash crumble", Dash, dx, -39.0f, 0.0f, 7, 8, 0.0f, 2, {vec3(0.25f, 0.72f, 0.78f), vec3(0.18f, 0.55f, 0.66f)}, false, true});
            f.push_back({"dash ring", Dash, gridCorner(DASH_X, 9), -25.0f, 0.0f, 9, 9, 8.4f, 2, {vec3(0.95f, 0.62f, 0.2f), vec3(0.92f, 0.88f, 0.78f)},
                         false, false});
            f.push_back({"dash checkpoint", Dash, dx, -25.0f + 9 * PITCH, 0.0f, 7, 2, 0.0f, 2, {white, grey}, true, false});
            f.push_back({"dash gaps", Dash, dx, -25.0f + 11 * PITCH, 0.0f, 7, 12, 0.0f, 2, {vec3(0.62f, 0.38f, 0.82f), vec3(0.46f, 0.28f, 0.66f)},
                         false, false, dashGaps});
            f.push_back({"dash checkpoint two", Dash, dx, -25.0f + 23 * PITCH, 0.0f, 7, 2, 0.0f, 2, {white, grey}, true, false});
            f.push_back({"dash narrow", Dash, dx, -25.0f + 25 * PITCH, 0.0f, 7, 8, 0.0f, 2, {vec3(0.32f, 0.66f, 0.3f), vec3(0.25f, 0.55f, 0.24f)},
                         false, false, dashNarrow});
            f.push_back({"dash finish", Dash, dx, -25.0f + 33 * PITCH, 0.0f, 7, 4, 0.0f, 2, {vec3(1.0f, 0.8f, 0.2f), vec3(0.92f, 0.68f, 0.12f)}, true,
                         false});
            return f;
        }();
        return specs;
    }

    // The voxel corner of a floor's component: where its voxel (0, 0, 0) stands.
    inline vec3 floorCorner(const FloorSpec& f) { return vec3(f.x0, f.top - float(f.thick) * VOXEL, f.z0); }

    struct TileSpec {
        int  i = 0, j = 0;    // in the floor's grid
        vec3 centre{0.0f};    // the middle of its top face, in the world
        bool present = true;  // in the files; a gap to build in if not
    };

    inline std::vector<TileSpec> tilesOf(const FloorSpec& f) {
        std::vector<TileSpec> tiles;
        const vec3 corner = floorCorner(f);
        const vec3 middle(f.x0 + float(f.nx) * PITCH * 0.5f, 0.0f, f.z0 + float(f.nz) * PITCH * 0.5f);
        for (int j = 0; j < f.nz; j++)
            for (int i = 0; i < f.nx; i++) {
                const vec3 centre = corner + vec3((float(i * PITCH_VOXELS) + TILE_VOXELS * 0.5f) * VOXEL, float(f.thick) * VOXEL,
                                                  (float(j * PITCH_VOXELS) + TILE_VOXELS * 0.5f) * VOXEL);
                if (f.radius > 0.0f && glm::length(vec3(centre.x - middle.x, 0.0f, centre.z - middle.z)) > f.radius) continue;
                const Cell cell = f.cell ? f.cell(i, j) : Solid;
                if (cell == None) continue;
                tiles.push_back({i, j, centre, cell == Solid});
            }
        return tiles;
    }
    // A tile's anchor voxel, in its floor's voxel space: the middle of its underside.
    inline ivec3 anchorVoxel(const TileSpec& t) { return ivec3(t.i * PITCH_VOXELS + 3, 0, t.j * PITCH_VOXELS + 3); }
    // Its top face, which turns red as it cracks.
    inline std::vector<ivec3> topVoxels(const FloorSpec& f, const TileSpec& t) {
        std::vector<ivec3> top;
        for (int z = 0; z < TILE_VOXELS; z++)
            for (int x = 0; x < TILE_VOXELS; x++) top.push_back(ivec3(t.i * PITCH_VOXELS + x, f.thick - 1, t.j * PITCH_VOXELS + z));
        return top;
    }
    // What a cracking tile's top turns: hot, then hotter.
    inline uint32_t crackColour(int stage) { return stage == 0 ? rgb(0.95f, 0.45f, 0.2f) : rgb(0.85f, 0.15f, 0.1f); }
    const uint32_t ANCHOR_COLOUR = rgb(0.12f, 0.12f, 0.13f);

    // A tile's voxels, in its floor's voxel space, its anchor among them: in the floor's checker, or
    // (a tile someone laid) in `colour`.
    inline std::vector<projv::PendingVoxelOp> tileVoxels(const FloorSpec& f, const TileSpec& t, const vec3* colour = nullptr) {
        std::vector<projv::PendingVoxelOp> ops;
        for (int y = 0; y < f.thick; y++)
            for (int z = 0; z < TILE_VOXELS; z++)
                for (int x = 0; x < TILE_VOXELS; x++) {
                    const ivec3 v(t.i * PITCH_VOXELS + x, y, t.j * PITCH_VOXELS + z);
                    const vec3 c = (colour ? *colour : f.colour[(t.i + t.j) % 2]) * shade(x, y, z, 0.12f) * (y == f.thick - 1 ? 1.0f : 0.72f);
                    ops.push_back({true, v, v == anchorVoxel(t) ? ANCHOR_COLOUR : rgb(c.x, c.y, c.z)});
                }
        return ops;
    }

    // ---- Models ----------------------------------------------------------------------------------------

    // A model being built: voxels, each colour naming the physics material it is made of.
    struct Model {
        std::string name;
        vec3        at{0.0f};            // where its voxel (0, 0, 0) corner stands
        float       voxel = 0.25f;
        std::vector<projv::PendingVoxelOp> ops;
        std::map<uint32_t, std::string> materials;   // colour -> physics material

        void put(int x, int y, int z, uint32_t colour, const std::string& material) {
            ops.push_back({true, ivec3(x, y, z), colour});
            if (!material.empty()) materials[colour] = material;
        }
    };

    inline Model floorModel(const FloorSpec& f) {
        Model m{f.name, floorCorner(f), VOXEL};
        for (const TileSpec& t : tilesOf(f)) {
            if (!t.present) continue;
            for (const projv::PendingVoxelOp& op : tileVoxels(f, t))
                m.put(op.position.x, op.position.y, op.position.z, op.packedColor, op.packedColor == ANCHOR_COLOUR ? "anchor" : "tile");
        }
        return m;
    }

    // ---- Building plots ----------------------------------------------------------------------------------
    //
    // Where players build in metre cubes (four voxels a side): Sky Climb's ground, Blueprint's pads and
    // its display. Each is one splitting component: a platform of stone half a metre thick, its
    // underside an anchor, on a grid of cells from its corner. A blast takes voxels out, and what it
    // cuts loose falls. The platform itself does not break.
    constexpr int CUBE = 4, PLATFORM = 2;   // voxels: a cube's side, the platform's depth
    constexpr vec3 PLAYER_COLOURS[MAX_PLAYERS] = {vec3(0.9f, 0.22f, 0.18f), vec3(0.2f, 0.42f, 0.95f), vec3(0.25f, 0.8f, 0.3f),
                                                  vec3(0.98f, 0.82f, 0.15f)};

    struct PlotSpec {
        std::string name;
        Mode        mode;
        vec3        centre;      // the middle of its top
        int         cells;       // across, each way
        int         owner;       // the player slot it is whose (its rim in their colour), -1 nobody's
        int         layers;      // cubes may stack this high
        bool        locked;      // nobody builds on it: Blueprint's display
    };
    inline vec3 plotCorner(const PlotSpec& p) {
        const float half = float(p.cells) * 0.5f;
        return p.centre - vec3(half, float(PLATFORM) * VOXEL, half);
    }

    // Sky Climb: one plot everyone builds on; the first to stand this high wins.
    constexpr float CLIMB_GOAL = 12.0f;
    constexpr int   CLIMB_CELLS = 12;
    // Blueprint: a pad each round a display, on a plaza; the shape to copy fills the middle 3 by 3 of a
    // pad, up to three high.
    constexpr int   PAD_CELLS = 5, SHAPE_CELLS = 3, SHAPE_LAYERS = 3;
    constexpr float PAD_OUT = 8.5f;   // m from the plaza's middle to a pad's
    inline vec3 padCentre(int slot) {
        static const vec3 dirs[MAX_PLAYERS] = {vec3(1, 0, 0), vec3(0, 0, 1), vec3(-1, 0, 0), vec3(0, 0, -1)};
        return arenaCentre(Blueprint) + dirs[slot] * PAD_OUT;
    }

    // Every plot. The server and the asset writer both read this.
    inline const std::vector<PlotSpec>& plotSpecs() {
        static const std::vector<PlotSpec> specs = [] {
            std::vector<PlotSpec> p;
            p.push_back({"climb ground", Climb, arenaCentre(Climb), CLIMB_CELLS, -1, 16, false});
            for (int slot = 0; slot < MAX_PLAYERS; slot++) p.push_back({"blueprint pad " + std::to_string(slot), Blueprint, padCentre(slot), PAD_CELLS, slot, 4, false});
            p.push_back({"blueprint display", Blueprint, arenaCentre(Blueprint), PAD_CELLS, -1, 4, true});
            return p;
        }();
        return specs;
    }
    // Which plot is a player's own in a game: its pad; -1 none.
    inline int ownPlot(Mode mode, int slot) {
        const auto& specs = plotSpecs();
        for (size_t i = 0; i < specs.size(); i++)
            if (specs[i].mode == mode && specs[i].owner == slot) return int(i);
        return -1;
    }
    inline int plotNamed(Mode mode, bool locked = false) {
        const auto& specs = plotSpecs();
        for (size_t i = 0; i < specs.size(); i++)
            if (specs[i].mode == mode && specs[i].owner < 0 && specs[i].locked == locked) return int(i);
        return -1;
    }

    // The plot of a game a point is over, and the cell of it (x, layer, z): -1 if none.
    inline int plotCellAt(Mode mode, vec3 at, ivec3& cell) {
        const auto& specs = plotSpecs();
        for (size_t i = 0; i < specs.size(); i++) {
            if (specs[i].mode != mode) continue;
            const vec3 local = at - plotCorner(specs[i]);
            if (local.x < 0.0f || local.z < 0.0f || local.x >= float(specs[i].cells) || local.z >= float(specs[i].cells)) continue;
            cell = ivec3(int(local.x), int(std::floor(local.y - float(PLATFORM) * VOXEL)), int(local.z));
            return int(i);
        }
        return -1;
    }
    // The middle of a plot's cell, in the world.
    inline vec3 cellCentre(int plot, ivec3 cell) {
        return plotCorner(plotSpecs()[size_t(plot)]) + vec3(float(cell.x), float(PLATFORM) * VOXEL + float(cell.y), float(cell.z)) + vec3(0.5f);
    }

    // The voxels of the cube in `cell` (x, layer, z) of a plot, its layer 0 on the platform.
    inline ivec3 cubeOrigin(ivec3 cell) { return ivec3(cell.x * CUBE, PLATFORM + cell.y * CUBE, cell.z * CUBE); }

    inline Model plotModel(const PlotSpec& p) {
        Model m{p.name, plotCorner(p), VOXEL};
        const int n = p.cells * CUBE;
        const vec3 rim = p.owner >= 0 ? PLAYER_COLOURS[p.owner] * 0.8f : (p.locked ? vec3(1.0f, 0.8f, 0.2f) : vec3(0.35f, 0.35f, 0.38f));
        for (int z = 0; z < n; z++)
            for (int x = 0; x < n; x++) {
                m.put(x, 0, z, ANCHOR_COLOUR, "anchor");
                // Stone, with a rim in its owner's colour; a grid of cells marked faintly.
                const bool edge = x == 0 || z == 0 || x == n - 1 || z == n - 1;
                const float s = shade(x, 1, z, 0.15f) * ((x / CUBE + z / CUBE) % 2 ? 1.0f : 0.92f);
                m.put(x, 1, z, edge ? rgb(rim.x, rim.y, rim.z) : rgb(0.6f * s, 0.58f * s, 0.55f * s), "stone");
            }
        return m;
    }

    // Sky Climb's goal: four striped posts at the ground's corners, gold from the goal's height up.
    inline Model climbPosts() {
        const float half = float(CLIMB_CELLS) * 0.5f + 0.75f;
        Model m{"climb posts", arenaCentre(Climb) - vec3(half, 0.0f, half), 0.5f};
        const int span = int(half * 2.0f / 0.5f), top = int((CLIMB_GOAL + 1.0f) / 0.5f), gold = int(CLIMB_GOAL / 0.5f);
        for (int x : {0, span})
            for (int z : {0, span})
                for (int y = 0; y < top; y++)
                    m.put(x, y, z, y >= gold ? rgb(1.0f, 0.82f, 0.2f) : ((y / 2) % 2 ? rgb(0.95f, 0.95f, 0.92f) : rgb(0.3f, 0.3f, 0.34f)), "");
        return m;
    }

    // Blueprint's plaza: 24 m of pale stone round the pads and the display, with holes where they sit.
    inline Model plaza() {
        const vec3 centre = arenaCentre(Blueprint);
        Model m{"blueprint plaza", centre - vec3(12.0f, 0.5f, 12.0f), 0.5f};
        for (int z = 0; z < 48; z++)
            for (int x = 0; x < 48; x++) {
                const vec3 at = m.at + vec3((float(x) + 0.5f) * 0.5f, 0.0f, (float(z) + 0.5f) * 0.5f);
                bool hole = std::abs(at.x - centre.x) < PAD_CELLS * 0.5f && std::abs(at.z - centre.z) < PAD_CELLS * 0.5f;
                for (int slot = 0; slot < MAX_PLAYERS; slot++) {
                    const vec3 pad = padCentre(slot);
                    hole = hole || (std::abs(at.x - pad.x) < PAD_CELLS * 0.5f && std::abs(at.z - pad.z) < PAD_CELLS * 0.5f);
                }
                if (hole) continue;
                const float s = shade(x, 0, z, 0.12f);
                m.put(x, 0, z, (x / 2 + z / 2) % 2 ? rgb(0.82f * s, 0.8f * s, 0.76f * s) : rgb(0.74f * s, 0.72f * s, 0.69f * s), "");
            }
        return m;
    }

    // A cube someone builds, in their colour.
    inline std::vector<projv::PendingVoxelOp> cubeVoxels(ivec3 cell, vec3 colour) {
        std::vector<projv::PendingVoxelOp> ops;
        const ivec3 o = cubeOrigin(cell);
        for (int y = 0; y < CUBE; y++)
            for (int z = 0; z < CUBE; z++)
                for (int x = 0; x < CUBE; x++) {
                    const bool edge = (x == 0 || x == CUBE - 1) + (y == 0 || y == CUBE - 1) + (z == 0 || z == CUBE - 1) >= 2;
                    const vec3 c = colour * (edge ? 0.6f : 0.95f);
                    ops.push_back({true, o + ivec3(x, y, z), rgb(c.x, c.y, c.z)});
                }
        return ops;
    }

    // The lava sea under the arenas: 400 by 120 m, glowing. Touch it and you are out.
    inline Model lava() {
        Model m{"lava", vec3(-200.0f, LAVA_Y - 1.0f, -60.0f), 1.0f};
        for (int z = 0; z < 120; z++)
            for (int x = 0; x < 400; x++) {
                const float wave = 0.5f + 0.5f * std::sin(float(x) * 0.42f + std::sin(float(z) * 0.34f) * 2.0f);
                const float s = shade(x, 0, z, 0.25f);
                m.put(x, 0, z, wave > 0.8f ? rgb(1.0f * s, 0.72f * s, 0.2f * s) : rgb(0.85f * s, 0.25f * s, 0.06f * s), "");
            }
        return m;
    }

    // Sky Dash's finish: an arch of gold and white over the line.
    inline Model finishArch() {
        Model m{"dash arch", vec3(gridCorner(DASH_X, 7) - 0.5f, 0.0f, DASH_FINISH - 0.25f), 0.25f};
        const int width = int((7 * PITCH + 1.0f) / 0.25f);
        for (int y = 0; y < 22; y++)
            for (int x = 0; x < width; x++) {
                const bool post = x < 3 || x >= width - 3;
                if (!post && y < 19) continue;
                m.put(x, y, 0, ((x / 3 + y / 3) % 2) ? rgb(1.0f, 0.82f, 0.2f) : rgb(0.95f, 0.95f, 0.92f), "");
                m.put(x, y, 1, ((x / 3 + y / 3) % 2) ? rgb(1.0f, 0.82f, 0.2f) : rgb(0.95f, 0.95f, 0.92f), "");
            }
        return m;
    }

    // Sky Dash's moving parts, which the server turns and slides: a sweeper, a bar across the ring that
    // turns about its middle (its pivot), knee high -- jump it; a pusher, a block that slides across
    // the narrow way.
    constexpr float SWEEPER_LENGTH = 15.0f, SWEEPER_HEIGHT = 0.45f;
    inline Model sweeper() {
        Model m{"sweeper", vec3(0.0f), 0.25f};
        const int n = int(SWEEPER_LENGTH / 0.25f);
        for (int x = 0; x < n; x++)
            for (int y = 0; y < 2; y++)
                for (int z = 0; z < 2; z++) m.put(x, y, z, (x / 4) % 2 ? rgb(0.9f, 0.15f, 0.12f) : rgb(0.95f, 0.95f, 0.92f), "");
        return m;
    }
    constexpr float PUSHER_HALF = 0.75f;
    inline Model pusher() {
        Model m{"pusher", vec3(0.0f), 0.25f};
        for (int y = 0; y < 6; y++)
            for (int z = 0; z < 4; z++)
                for (int x = 0; x < 6; x++) {
                    const bool stripe = (x + y) % 4 < 2;
                    m.put(x, y, z, stripe ? rgb(0.95f, 0.75f, 0.1f) : rgb(0.15f, 0.15f, 0.17f), "");
                }
        return m;
    }

    // A rock pillar rising out of the lava under each arena's edge: scenery, to give the drop a depth.
    inline Model pillar(const std::string& name, vec3 at, float height, int seed) {
        Model m{name, at, 0.5f};
        const int h = int(height / 0.5f);
        for (int y = 0; y < h; y++) {
            const float r = 2.2f + 1.0f * std::sin(float(y) * 0.3f + float(seed)) * (1.0f - float(y) / float(h));
            for (int z = -6; z <= 6; z++)
                for (int x = -6; x <= 6; x++) {
                    if (float(x * x + z * z) > r * r * 4.0f) continue;
                    const float s = shade(x, y, z + seed, 0.3f);
                    m.put(x + 6, y, z + 6, rgb(0.24f * s, 0.2f * s, 0.2f * s), "");
                }
        }
        return m;
    }

    // A holding pen far below and away, where the out wait for the next round. It does not break.
    inline Model pen() {
        Model m{"pen", vec3(-5.0f, -80.0f, 145.0f), 0.5f};
        for (int y = 0; y < 6; y++)
            for (int z = 0; z < 20; z++)
                for (int x = 0; x < 20; x++)
                    if (y == 0 || x == 0 || z == 0 || x == 19 || z == 19) m.put(x, y, z, rgb(0.3f, 0.3f, 0.32f), "");
        return m;
    }
    constexpr float PEN_FLOOR = -79.5f;
    inline vec3 penCentre() { return vec3(0.0f, PEN_FLOOR, 150.0f); }

    // A player: a round little figure in its own colour, its feet on its pivot, facing +z.
    inline Model player(int index) {
        Model m{"player " + std::to_string(index), vec3(0.0f), 0.125f};
        const vec3 c = PLAYER_COLOURS[index];
        for (int y = 0; y < 14; y++)
            for (int z = 0; z < 5; z++)
                for (int x = 0; x < 5; x++) {
                    // Rounded: the corners off.
                    if ((x == 0 || x == 4) && (z == 0 || z == 4)) continue;
                    uint32_t colour = rgb(c.x, c.y, c.z);
                    if (y < 3) colour = rgb(0.18f, 0.18f, 0.2f);                                       // boots
                    else if (y >= 9 && y < 13) colour = rgb(0.95f, 0.82f, 0.68f);                      // face
                    else if (y >= 13) colour = rgb(c.x * 0.7f, c.y * 0.7f, c.z * 0.7f);                // a cap
                    if (y == 11 && z == 4 && (x == 1 || x == 3)) colour = rgb(0.08f, 0.08f, 0.08f);    // eyes
                    m.put(x, y, z, colour, "");
                }
        return m;
    }

    // A bomb: a black iron ball with a red band and a fuse, its pivot the middle.
    inline Model bomb() {
        Model m{"bomb", vec3(0.0f), 0.1f};
        for (int z = 0; z < 7; z++)
            for (int y = 0; y < 9; y++)
                for (int x = 0; x < 7; x++) {
                    const vec3 d = vec3(float(x), float(y), float(z)) + 0.5f - vec3(3.5f);
                    if (y < 7 && glm::length(d) <= 3.5f) m.put(x, y, z, y == 3 ? rgb(0.85f, 0.12f, 0.1f) : rgb(0.1f, 0.1f, 0.12f), "iron");
                    else if (y >= 7 && x == 3 && z == 3) m.put(x, y, z, y == 8 ? rgb(1.0f, 0.85f, 0.3f) : rgb(0.6f, 0.5f, 0.35f), "iron");
                }
        return m;
    }

    // ---- Writing the scene folder ---------------------------------------------------------------------

    constexpr const char* FOLDER = "party";
    constexpr const char* MATERIALS = "physics_materials.json";
    inline std::string playerPrefab(int index) { return "player_" + std::to_string(index); }
    constexpr const char* BOMB = "bomb";
    constexpr const char* SWEEPER = "sweeper";
    constexpr const char* PUSHER = "pusher";

    constexpr const char* MATERIAL_LIBRARY = R"({
  "v": 1,
  "materials": [
    { "name": "tile",   "density": 900,  "friction": 0.7, "restitution": 0.0 },
    { "name": "anchor", "density": 900,  "friction": 0.7, "restitution": 0.0, "anchor": true },
    { "name": "stone",  "density": 2200, "friction": 0.8, "restitution": 0.0 },
    { "name": "iron",   "density": 3000, "friction": 0.6, "restitution": 0.3 }
  ]
})";

    // A model as a top-level component of `scene`, standing where its `at` says, each palette entry
    // naming its physics material.
    inline projv::ComponentHandle place(projv::Scene& scene, const Model& m) {
        using namespace projv;
        ComponentHandle h = utils::addComponent(scene, ComponentKind::Chunk, m.name, INVALID_COMPONENT_HANDLE, 16, m.voxel);
        utils::queueVoxelAdd(scene, h, m.ops);
        utils::updateScene(scene);
        utils::setComponentTransform(scene, h, m.at, quat(1, 0, 0, 0), 1.0f);
        const auto& palette = scene.components[h].materialPalette;
        for (size_t slot = 0; slot < palette.size(); slot++)
            if (auto it = m.materials.find(palette[slot].packedColor); it != m.materials.end())
                utils::setMaterialPhysics(scene, h, uint8_t(slot), it->second);
        return h;
    }

    // A prefab: the model alone in a folder, offset so `pivot` (in voxels) is the prefab's origin.
    inline bool writePrefab(const std::filesystem::path& folder, Model m, vec3 pivot) {
        projv::Scene scene;
        m.at = -pivot * m.voxel;
        place(scene, m);
        return projv::utils::saveComposeToDisk(scene, projv::INVALID_COMPONENT_HANDLE, folder.string());
    }

    // The arenas, the lava and the pen as one scene; and its entities.json: the floors and the plots
    // solid, splitting (what loses its anchor falls) and named by a NetId so their changes reach every
    // client; the rest solid.
    inline bool writeField(const std::filesystem::path& folder) {
        struct Entry { Model model; bool floor; };
        std::vector<Entry> entries{{lava(), false}, {pen(), false}};
        for (const FloorSpec& f : floorSpecs()) entries.push_back({floorModel(f), true});
        for (const PlotSpec& p : plotSpecs()) entries.push_back({plotModel(p), true});
        entries.push_back({finishArch(), false});
        entries.push_back({climbPosts(), false});
        entries.push_back({plaza(), false});
        int seed = 0;
        for (int m = 0; m < MODES; m++)
            for (float side : {-1.0f, 1.0f}) {
                const vec3 at = arenaCentre(Mode(m)) + vec3(side * 9.0f - 3.0f, LAVA_Y, side * 11.0f - 3.0f);
                entries.push_back({pillar("pillar " + std::to_string(seed), at, 18.0f + 4.0f * float(seed % 3), seed), false});
                seed++;
            }

        projv::Scene scene;
        for (const Entry& e : entries) place(scene, e.model);
        if (!projv::utils::saveComposeToDisk(scene, projv::INVALID_COMPONENT_HANDLE, folder.string())) return false;
        const projv::Scene saved = projv::utils::loadComposeFromDisk(folder.string());
        std::ofstream out(folder / "entities.json");
        out << "{ \"version\": 1, \"entities\": [\n";
        for (size_t i = 0; i < entries.size(); i++) {
            const std::string components = entries[i].floor
                ? R"({ "physics.static": { "v": 1, "splits": true }, "projv.netid": { "v": 1, "id": )" + std::to_string(10 + i) + " } }"
                : std::string(R"({ "physics.static": { "v": 1 } })");
            out << "  { \"name\": \"" << entries[i].model.name << "\", \"link\": " << saved.components[i].localId
                << ", \"components\": " << components << " }" << (i + 1 < entries.size() ? ",\n" : "\n");
        }
        out << "] }\n";
        return bool(out);
    }

    inline bool write(const std::string& where) {
        const std::filesystem::path folder(where);
        std::filesystem::create_directories(folder);
        if (!writeField(folder)) return false;
        std::ofstream(folder / MATERIALS) << MATERIAL_LIBRARY;
        bool ok = true;
        for (int i = 0; i < MAX_PLAYERS; i++) ok = ok && writePrefab(folder / playerPrefab(i), player(i), vec3(2.5f, 0.0f, 2.5f));
        ok = ok && writePrefab(folder / BOMB, bomb(), vec3(3.5f));
        ok = ok && writePrefab(folder / SWEEPER, sweeper(), vec3(SWEEPER_LENGTH / 0.25f * 0.5f, 0.0f, 1.0f));
        ok = ok && writePrefab(folder / PUSHER, pusher(), vec3(3.0f, 0.0f, 2.0f));
        return ok;
    }
}
