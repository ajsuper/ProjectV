// Explosion -- a big one, three ways: a city, a house on a suburban street, and a pine forest (maps.h), each
// blown apart from its middle, by day or on a dark night.
//
// Two things happen at once when it goes up:
//
//   The real explosion   everything is static voxel grids whose colours name their physics materials --
//                        concrete, brick, plaster, glass, timber, wood, needles. A crater is dug under
//                        ground zero; then the shock front runs out and acts on each thing once, as it
//                        gets there (shockSweep). A structure near ground zero is taken apart whole -- one
//                        strike that reaches every voxel, broken by the "demolish" or "rubble" pattern
//                        into slabs, wall panels and rubble -- and the pieces thrown out from ground zero;
//                        further out its side facing the blast is torn out, and what falls for want of it
//                        is broken up as it goes; further still, its windows facing the blast go. A tree
//                        near ground zero is shattered; further out the wind behind the front knocks it
//                        flat, away from the blast (stripped bare nearer in), and further still bends it
//                        hard over. All of it is ProjectV's physics -- Jolt bodies, fracture, collapse --
//                        and voxel edits.
//
//   The mushroom cloud   a quasi-simulation. A velocity field shaped like a mushroom cloud (mushroom.h: a
//                        rising vortex-ring cap, a stem, an inflow along the ground) carries thousands of
//                        embers up out of the ruins. Each is a plain particle, not a physics body, drawn
//                        as a cluster of glowing voxel lumps (traced by the G-buffer: night_renderer.h)
//                        that starts white-hot, cools through red to charcoal, and turns to smoke.
//
// Rendered with Voxel Mayhem's renderer (examples/shared/voxelRenderer) given a night beside its day, the
// explosion as a second shadowed light, the embers, and a smoke pass: nightRenderer/, night_renderer.h.
//
// Command line
//   --map city|house|forest      which map (city by default; 1, 2, 3 switch while it runs)
//   --graphics 1|2|3             1 direct light and SSAO (no shadows or GI), 2 that and shadows (the sun's or
//                                moon's and the fire's), 3 everything: the bounce, ray-traced GI too (default)
//
// Keys
//   Space  detonate (it also goes off by itself 4 s after a build)      R  rebuild   1 2 3  city, house, forest
//   F      fly (mouse, W/A/S/D, Space/Shift; Esc frees the cursor) / the orbiting camera
//   Left/Right, Up/Down   turn the orbit, nearer/farther                  T  slow motion   P  pause
//   M  the smoke: pure voxels (embers become smoke puffs) / the grid as voxel blocks / soft volumetric gas
//   E  the ember chunks   H  shadows   I  ray-traced GI   K  TAA   N  day / night   [ ]  exposure   F1  help
//
// Environment (unattended runs)
//   EXPLOSION_LOOK=x,y,z,tx,ty,tz   a fixed camera
//   EXPLOSION_SHOTS=t1,t2,...       screenshots at those seconds after the start (EXPLOSION_CAPTURE=<prefix>)
//   EXPLOSION_EXIT=t                quit at t seconds          EXPLOSION_DETONATE=t   when it goes off
//   EXPLOSION_STATS=1               a line a second: fps, bodies, embers, and where the frame went
//   EXPLOSION_GPU=1                 each render pass's GPU time, averaged over a second, every second
//   EXPLOSION_SMOKE=0|1|2           the smoke mode to start in
//   EXPLOSION_NIGHT=1               night instead of day          EXPLOSION_EXPOSURE=x   the base exposure
//   EXPLOSION_EMBERS=x              every ember count and rate, times x (1.5 by default)
//   EXPLOSION_TIMESCALE=s           slow motion from the start    EXPLOSION_SIZE=WxH     the window
//   EXPLOSION_RECORD=fps            record: every frame advances the simulation exactly 1/fps s (however long
//                                   the frame takes -- slow motion for the machine, real time for the video)
//                                   and is saved as <EXPLOSION_CAPTURE>_00000.tga, _00001, ... from
//                                   EXPLOSION_RECORD_FROM seconds (half a second before the detonation) to
//                                   EXPLOSION_EXIT. ffmpeg -framerate <fps> makes it a real-time video.
//   EXPLOSION_PIECES=n, EXPLOSION_SETTLE=0, EXPLOSION_THREADS=n, EXPLOSION_NOCRATER=1, EXPLOSION_VERBOSE=1

#include <filesystem>

#include <bgfx/bgfx.h>
#include <GLFW/glfw3.h>

#include "core/log.h"
#include "core/paths.h"
#include "graphics/input.h"
#include "graphics/render_instance.h"

#include "explosion.h"
#include "maps.h"

namespace fs = std::filesystem;
using namespace explosion;
using night_renderer::Look;

namespace {

int graphicsLevel = 3;                       // --graphics
const MapSpec* startingMap = &MAPS[0];       // --map

// The ember chunk's colours as it cools: stage by stage from the fireball's yellow-orange to charcoal.
struct Stage { float above; vec3 colour; float glow; };
const Stage STAGES[] = {
    {0.86f, {1.00f, 0.80f, 0.42f}, 34.0f},
    {0.72f, {1.00f, 0.62f, 0.20f}, 24.0f},
    {0.58f, {1.00f, 0.45f, 0.09f}, 16.0f},
    {0.45f, {0.95f, 0.32f, 0.05f}, 10.0f},
    {0.33f, {0.80f, 0.17f, 0.03f}, 5.5f},
    {0.22f, {0.50f, 0.07f, 0.02f}, 2.5f},
    {0.13f, {0.24f, 0.07f, 0.03f}, 0.8f},
    {-1.0f, {0.12f, 0.11f, 0.105f}, 0.0f},
};
constexpr int STAGE_COUNT = int(std::size(STAGES));
constexpr int PUFF_SHADES = 3;
const vec3 SOOT[PUFF_SHADES] = {{0.13f, 0.125f, 0.12f}, {0.2f, 0.195f, 0.19f}, {0.28f, 0.275f, 0.27f}};
const vec3 DUST_PUFF[PUFF_SHADES] = {{0.42f, 0.36f, 0.28f}, {0.48f, 0.43f, 0.36f}, {0.52f, 0.49f, 0.44f}};
const vec3 VAPOUR_PUFF[PUFF_SHADES] = {{0.62f, 0.65f, 0.7f}, {0.7f, 0.73f, 0.78f}, {0.78f, 0.8f, 0.84f}};

struct Camera {
    vec3 position{-160.0f, 30.0f, -160.0f};
    float yaw = 0.8f, pitch = -0.05f;
    bool fly = false;
    float orbit = -2.33f, distance = 230.0f;   // the orbit's angle about ground zero, and how far out
    vec3 target{0.0f, 15.0f, 0.0f};
    vec3 forward() const { return {std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw)}; }
};

struct Shake { float trauma = 0.0f, clock = 0.0f; };

// How many voxels a piece is made of, noted when it breaks off (and again when it breaks further): what the
// piece budget (cullPieces) clears by, smallest first.
struct PieceSize { uint32_t voxels = 0; };

// How long a piece has lain still (settleTick).
struct Stillness { float seconds = 0.0f; };

// =================================================================================================
// The ground, and building and clearing a map
// =================================================================================================

// The ground: a metre of voxels, 2 floorHalf square, coloured by the map -- with a hole under ground zero,
// where a block of earth (earthHalf across, earthDepth deep, finer voxels) stands in for it, for the blast
// to dig a crater out of. The earth is held up by its own base (anchorBase), so what a crater leaves
// overhanging falls in.
void buildGround(World& world, Demo& d) {
    auto& scene = world.ctx().get<projv::Scene>();
    const MapSpec& m = *d.map;
    const int cells = int(2.0f * m.floorHalf);
    ComponentHandle floor = voxels(scene, "ground", ivec3(cells, 1, cells), 1.0f, vec3(-m.floorHalf, -1.0f, -m.floorHalf), [&](int x, int, int z) {
        const float wx = float(x) - m.floorHalf, wz = float(z) - m.floorHalf;
        if (wx >= -m.earthHalf && wx < m.earthHalf && wz >= -m.earthHalf && wz < m.earthHalf) return 0u;
        return m.ground(wx + 0.5f, wz + 0.5f, hashCell(x, 0, z));
    });
    d.floor = own(d, runtime::spawnComponent(world, floor));
    world.emplace<projv::StaticCollider>(d.floor);
    if (std::getenv("EXPLOSION_NOCRATER")) return;
    const float v = m.earthVoxel;
    const int across = int(2.0f * m.earthHalf / v), deep = int(m.earthDepth / v);
    projv::StaticCollider earth = breakable("earth");
    d.earth = own(d, scenery(world, "earth", ivec3(across, deep, across), v, vec3(-m.earthHalf, -m.earthDepth, -m.earthHalf),
                             [&](int x, int y, int z) -> uint32_t {
                                 const uint32_t h = hashCell(x, y, z);
                                 const int below = deep - 1 - y;
                                 if (below == 0) {   // the top: the ground's own colour
                                     const float wx = float(x) * v - m.earthHalf, wz = float(z) * v - m.earthHalf;
                                     return m.ground(wx + 0.5f * v, wz + 0.5f * v,
                                                     hashCell(int(std::floor(wx + m.floorHalf)), 0, int(std::floor(wz + m.floorHalf))));
                                 }
                                 if (below < int(1.5f / v)) return h % 3 ? rgb(0.2f, 0.14f, 0.09f) : rgb(0.17f, 0.12f, 0.08f);
                                 if (h % 23 == 0) return rgb(0.35f, 0.34f, 0.32f);   // a stone
                                 return h % 2 ? rgb(0.3f, 0.21f, 0.13f) : rgb(0.27f, 0.19f, 0.12f);
                             }, earth));
}

void clearWorld(World& world) {
    auto& d = world.ctx().get<Demo>();
    std::vector<Entity> doomed;
    for (Entity e : world.view<projv::RigidBody>()) doomed.push_back(e);
    for (Entity e : world.view<projv::Piece>()) doomed.push_back(e);
    doomed.insert(doomed.end(), d.owned.begin(), d.owned.end());
    std::sort(doomed.begin(), doomed.end());
    doomed.erase(std::unique(doomed.begin(), doomed.end()), doomed.end());
    for (Entity e : doomed) if (e != NullEntity && world.valid(e)) world.destroy(e);
    d.trees.clear();
    d.treePalette = Palette{};
    d.groves.clear();
    d.leaning.clear();
    d.owned.clear();
    d.targets.clear();
    d.breaking.clear();
    d.cloud.clear();
    d.kicks.clear();
    d.newborn.clear();
    d.floor = d.earth = d.groundZero = NullEntity;
    d.detonated = false;
    d.boomStep = 0;
    d.pieces = 0;
}

// Builds `map`: its ground and earth, then what stands on them. Everything the explosion sizes by the map's
// scale is set here: ground zero, the cloud's field, the smoke grid's cells, the orbiting camera.
void buildWorld(World& world, const MapSpec& map) {
    auto& d = world.ctx().get<Demo>();
    Look& look = world.ctx().get<Look>();
    Camera& camera = world.ctx().get<Camera>();
    const auto start = Clock::now();
    d.map = &map;
    d.S = map.scale;
    d.blast = vec3(0.0f, 1.6f * d.S, 0.0f);
    d.field = mushroom::Field{};
    d.field.scale = 2.0f * d.S;   // the city's cloud is twice the house-sized one this example began with
    look.smokeCell = 3.5f * d.S;
    look.smokeMin = vec3(-0.5f * float(night_renderer::GRID) * look.smokeCell, -1.0f, -0.5f * float(night_renderer::GRID) * look.smokeCell);
    camera.distance = map.orbitDistance;
    camera.target = vec3(0.0f, map.targetHeight, 0.0f);
    buildGround(world, d);
    map.build(world, d);
    std::printf("explosion: built %s -- %zu things to break in %.1f s\n", map.title, d.targets.size(), msSince(start) / 1000.0);
}

// =================================================================================================
// The explosion
// =================================================================================================

void addShake(World& world, float amount) {
    Shake& shake = world.ctx().get<Shake>();
    shake.trauma = std::min(1.0f, shake.trauma + amount);
}

// The shockwave's dust ring: its radius t seconds after the detonation, and how fast it is growing. It slows to
// a stop the map's shockReach out: in the city, ninety metres a second at first, stopping some hundred and fifty
// metres out; across the forest, twice that.
constexpr float SHOCK_TAU = 1.6f, SHOCK_HOLD = 4.5f;
float shockRadius(const Demo& d, float t) { return 3.0f * d.S + d.map->shockReach * (1.0f - std::exp(-t / SHOCK_TAU)); }
float shockSpeed(const Demo& d, float t) { return d.map->shockReach / SHOCK_TAU * std::exp(-t / SHOCK_TAU); }
// The condensation collar, t seconds after it forms (COLLAR_AT after the detonation).
constexpr float COLLAR_AT = 1.1f, COLLAR_REACH = 55.0f, COLLAR_TAU = 1.2f, COLLAR_HOLD = 4.5f;
float collarRadius(const Demo& d, float t) { return d.S * (10.0f + COLLAR_REACH * (1.0f - std::exp(-t / COLLAR_TAU))); }
float collarSpeed(const Demo& d, float t) { return d.S * COLLAR_REACH / COLLAR_TAU * std::exp(-t / COLLAR_TAU); }

void spawnEmber(Demo& d, vec3 at, vec3 launch, float temperature, Kind kind) {
    Ember em;
    em.position = em.previous = at;
    em.velocity = launch;
    em.kind = kind;
    em.temperature = temperature;
    em.cooling = kind == Kind::Fire ? uniform(d, 0.15f, 0.3f) : 0.0f;
    // Its cubes: the first at the middle, the rest about it.
    em.cubeCount = kind == Kind::Fire ? 5 + int(d.random() % 4) : 3 + int(d.random() % 2);
    const float edge = (kind == Kind::Fire ? 1.2f : kind == Kind::Dust ? 0.7f : 1.0f) * d.S;
    for (int i = 0; i < em.cubeCount; i++) {
        const vec3 offset = i == 0 ? vec3(0.0f) : inBall(d, 1.3f * d.S);
        em.cubes[size_t(i)] = vec4(offset, 0.5f * edge * uniform(d, 0.6f, 1.15f));
        em.seeds[size_t(i)] = uniform(d, 0.0f, 100.0f);
    }
    em.axis = unitVector(d);
    em.spin = uniform(d, -2.5f, 2.5f);
    em.born = float(d.boomStep) * d.stepSeconds;
    switch (kind) {
        case Kind::Fire:   em.smoke = uniform(d, 0.6f, 0.9f); break;
        case Kind::Dust:   em.smoke = uniform(d, 0.4f, 0.6f); em.chunkLife = uniform(d, 2.0f, 4.5f); break;
        case Kind::Shock:  em.smoke = uniform(d, 0.45f, 0.7f); em.chunkLife = uniform(d, 2.5f, 5.0f); break;
        case Kind::Collar: em.smoke = uniform(d, 0.5f, 0.8f); em.chunkLife = uniform(d, 2.0f, 3.5f); break;
    }
    // The collar's vapour and the ground's dust settle and clear long before the cloud does.
    em.lifetime = kind == Kind::Collar ? uniform(d, 9.0f, 12.0f) : kind == Kind::Fire ? uniform(d, 42.0f, 55.0f) : uniform(d, 9.0f, 16.0f);
    d.cloud.push_back(em);
}

void detonate(Demo& d) {
    if (d.detonated) return;
    d.detonated = true;
    d.boomStep = 0;
    projv::core::info("explosion: detonated");
}

// How far a target is from ground zero: to the nearest point of its footprint.
float distanceTo(const Demo& d, const Target& t) {
    const vec3 nearest = glm::clamp(d.blast, t.lo, t.hi);
    return glm::length(vec3(nearest.x - d.blast.x, 0.0f, nearest.z - d.blast.z));
}

// The blast's damage, dealt to each target once, as the shock front reaches it: ONE strike or edit a
// target. (It was a lattice of strikes every five metres through each building, but every strike on a body
// is an island pass over that whole body, and the front was firing a hundred a step: physics steps of
// 100-270 ms. Edits, by contrast, are gathered into one pass a body a step.) By distance, in the map's radii:
//
//   structures   inside flattenRadius   taken apart whole: a strike reaching every voxel, broken by its
//                                       floor's pattern ("demolish": a storey at a time; "rubble") into
//                                       pieces of flattenShard(distance), that onBrokenBody throws out
//                inside wreckRadius     wrecked: a strike from the middle of the face toward the blast,
//                                       reaching most of the way in at flattenRadius and a third of it at
//                                       wreckRadius. What stands on is a ruin; what falls for want of it is
//                                       broken up as it goes (onBrokenBody), so nothing comes down whole
//                inside glassRadius     the windows facing the blast are blown out
//   trees        inside shatterRadius   shattered: a strike reaching all of it
//                inside stripRadius     knocked flat, away from the blast, its crown stripped off: a bare trunk
//                inside fellRadius      knocked flat, crown and all
//                inside leanRadius      tipped hard over away from the blast, swaying in the gusts, some of its
//                                       needles blown off
//                The wind behind the front does all but the first, as edits of the tree's own grove (leanTick):
//                no bodies, so thousands of trees can go down at once.
//
// How fast the blast throws what it breaks, by distance: debris falls under real gravity, so on a smaller
// map it flies slower by the square root of the scale (as the cloud's field does).
float blastSpeed(const Demo& d, float distance) { return (75.0f * std::exp(-distance / (40.0f * d.S)) + 3.0f) * std::sqrt(d.S); }
// The size a structure breaks to across (a storey at a time up, in the city), metres, by its distance.
float flattenShard(const Demo& d, float distance) { return d.map->shardScale * (6.0f * d.S + 0.045f * distance); }
float shardFor(const Demo& d, const Target& t, float distance) { return t.shard > 0.0f ? t.shard : flattenShard(d, distance); }
// Every piece a break makes costs the step ~0.15 ms (its entity, component, body), and the maps are
// symmetric: four or eight buildings stand at the same distance and the front reaches them in the same step.
// So no more pieces than this are started in one step, the rest waiting a step or two (a target's estimate
// is a piece a cell: its storeys by its width and depth in shard sizes; a felled tree is one).
constexpr float BREAK_PIECES_PER_STEP = 250.0f;

// Fracture by a jittered grid: Voronoi cells round one jittered seed in each cell of a grid `size` voxels
// across, everything within the reach broken. The built-in patterns weigh every voxel against every seed and
// stop at 256 seeds; this looks only at the cells round each voxel, so one strike can take a whole tower apart
// into hundreds of pieces in a few milliseconds. With `layer` > 0 the grid is that many voxels tall and
// unjittered in y, its rows centred on y = 0, layer, 2 layer...: a building's storeys, so each piece holds
// one floor slab and the walls half a storey above and below it (cells that spanned two slabs came apart into
// two plates each, as the engine cuts a shard into its connected parts). Deterministic: the grid's offset and
// the seeds come from the strike's FractureRandom.
std::vector<int32_t> jitteredCells(const runtime::FractureInput& in, runtime::FractureRandom& random, float layer) {
    const float size = std::max(in.shardSize, 1.0f);
    const vec3 cell(size, layer > 0.0f ? layer : size, size);
    const vec3 origin(random.uniform(0.0f, size), layer > 0.0f ? -0.5f * layer : random.uniform(0.0f, size), random.uniform(0.0f, size));
    const int salt = int(random.next() & 0xffffff);
    const int reachY = layer > 0.0f ? 0 : 1;
    std::vector<int32_t> shard(in.voxels.size(), -1);
    std::map<std::tuple<int, int, int>, int32_t> ids;
    for (size_t i = 0; i < in.voxels.size(); i++) {
        const runtime::FractureVoxel& v = in.voxels[i];
        const vec3 c = vec3(v.position) + 0.5f;
        if (!v.breakable || glm::distance(c, in.at) > in.reach) continue;
        const vec3 g = (c - origin) / cell;
        const ivec3 home(glm::floor(g));
        float best = 1e30f;
        ivec3 nearest = home;
        for (int dz = -1; dz <= 1; dz++)
            for (int dy = -reachY; dy <= reachY; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    const ivec3 n = home + ivec3(dx, dy, dz);
                    const uint32_t h = hashCell(n.x + salt, n.y, n.z);
                    vec3 seed = vec3(n) + vec3(float(h & 1023u), float((h >> 10) & 1023u), float((h >> 20) & 1023u)) / 1023.0f;
                    if (layer > 0.0f) seed.y = g.y;
                    const vec3 to = g - seed;
                    if (const float d2 = glm::dot(to, to); d2 < best) { best = d2; nearest = n; }
                }
        auto [it, made] = ids.try_emplace({nearest.x, nearest.y, nearest.z}, int32_t(ids.size()));
        shard[i] = it->second;
    }
    return shard;
}
// City concrete: storey by storey. The earth and a house's floors: rubble, all ways alike.
std::vector<int32_t> demolish(const runtime::FractureInput& in, runtime::FractureRandom& random) { return jitteredCells(in, random, float(STOREY)); }
std::vector<int32_t> rubble(const runtime::FractureInput& in, runtime::FractureRandom& random) { return jitteredCells(in, random, 0.0f); }

// A strike from its strike point that takes all of the target: reaching its own reach, or its furthest corner.
void strikeWhole(World& world, const Demo& d, const Target& t, float distance) {
    runtime::Strike strike;
    strike.at = t.worldOf(vec3(t.strikeAt));
    float reach = 0.0f;
    for (int corner = 0; corner < 8; corner++) {
        const vec3 v((corner & 1) ? t.voxels.x : 0, (corner & 2) ? t.voxels.y : 0, (corner & 4) ? t.voxels.z : 0);
        reach = std::max(reach, glm::length(v - vec3(t.strikeAt)));
    }
    strike.reach = t.reach > 0.0f ? t.reach : reach * t.voxel + 1.0f;
    strike.shardSize = shardFor(d, t, distance);
    runtime::fracture(world, t.e, strike);
}

// Tears out the side of a structure facing the blast: a strike from the middle of that face, at the height of
// its strike point (a floor), reaching the further in the nearer it stands.
void wreck(World& world, const Demo& d, const Target& t, float distance) {
    const MapSpec& m = *d.map;
    const float f = std::clamp((distance - m.flattenRadius) / (m.wreckRadius - m.flattenRadius), 0.0f, 1.0f);
    const vec3 size = vec3(t.voxels) * t.voxel;
    vec3 face = glm::clamp(d.blast, t.lo + 0.3f, t.hi - 0.3f);
    face.y = t.worldOf(vec3(t.strikeAt)).y;
    runtime::Strike strike;
    strike.at = face;
    strike.reach = glm::mix(0.85f, 0.35f, f) * std::max({size.x, size.z, 0.7f * size.y});
    strike.shardSize = flattenShard(d, distance);
    runtime::fracture(world, t.e, strike);
}

// Blows out the windows on the faces of a structure that look toward the blast: one edit.
void blowOutWindows(World& world, const Demo& d, const Target& t) {
    const vec3 toBlast = d.blast - 0.5f * (t.lo + t.hi);
    std::vector<projv::PendingVoxelOp> ops;
    for (const ivec3& g : t.glass) {
        const vec3 normal(g.x == 0 ? -1.0f : g.x == t.voxels.x - 1 ? 1.0f : 0.0f, 0.0f,
                          g.z == 0 ? -1.0f : g.z == t.voxels.z - 1 ? 1.0f : 0.0f);
        if (glm::dot(normal, toBlast) > 0.0f) ops.push_back({false, g, 0u});
    }
    if (!ops.empty()) runtime::editVoxels(world, t.e, ops);
}

// The wind behind the front tips a tree over about the foot of its trunk, away from the blast, in LEAN_STAGES
// gusts LEAN_EVERY steps apart. A tree that goes down flat falls further at each gust; one that is only tipped
// goes past the angle it ends at and comes back. Each gust is one edit of the tree's grove: its voxels taken
// away and the tree put back turned -- each place in the turned tree's box takes the voxel it came from, so
// the turn leaves no holes -- with more of its needles blown off at each gust (all of them if `strip`). Only
// what is still joined to the stump is put back, so the tree stays one piece (a scrap of crown left hanging
// would come away as a body of its own), and only where its grove has nothing (Grove): where another tree
// lies, this one goes under it.
constexpr int LEAN_STAGES = 2, LEAN_EVERY = 5;
constexpr float FALL_GUSTS[LEAN_STAGES] = {0.5f, 1.0f}, SWAY_GUSTS[LEAN_STAGES] = {1.2f, 1.0f};
void tip(World& world, Demo& d, Target& t, const Demo::Lean& lean) {
    Grove& grove = d.groves[t.e];
    const std::vector<ivec3> was = std::move(t.now);
    for (const ivec3& v : was) grove.fill(v, false);
    t.now.clear();
    // The tree as it grew, in a box about its foot: u along the way it tips (away from the blast), w across.
    const int other = lean.axis == 0 ? 2 : 0;
    int reach = 0, height = 0;
    for (const TreeVoxel& b : t.body) {
        reach = std::max({reach, std::abs(b.at[lean.axis] - t.foot[lean.axis]), std::abs(b.at[other] - t.foot[other])});
        height = std::max(height, b.at.y + 1);
    }
    const int side = 2 * reach + 1;
    std::vector<int32_t> grown(size_t(side) * size_t(side) * size_t(height), -1);   // index into body
    auto cell = [&](int u, int y, int w) { return (size_t(y) * size_t(side) + size_t(w + reach)) * size_t(side) + size_t(u + reach); };
    for (size_t i = 0; i < t.body.size(); i++) {
        const ivec3 a = t.body[i].at;
        grown[cell(lean.sign * (a[lean.axis] - t.foot[lean.axis]), a.y, a[other] - t.foot[other])] = int32_t(i);
    }
    const float angle = lean.angle * (lean.falls ? FALL_GUSTS : SWAY_GUSTS)[lean.stage];
    const float c = std::cos(angle), s = std::sin(angle);
    const int stripped = lean.strip ? 100 : 15 + 15 * lean.stage;   // of a hundred needles, blown off by now
    // Every place in the turned box takes the voxel it turned from (if any, and if it is not blown off). The
    // box: the grown box's corners, turned -- u from uLo to uHi, y up to yHi.
    const int uLo = int(std::floor(-float(reach) * c)) - 1, uHi = int(std::ceil(float(reach) * c + float(height) * s)) + 1;
    const int yHi = int(std::ceil(float(reach) * s + float(height) * c)) + 1;
    const int wide = uHi - uLo + 1;
    std::vector<int32_t> turned(size_t(wide) * size_t(yHi + 1) * size_t(side), -1);   // place -> index into body
    std::vector<uint8_t> joined(turned.size(), 0);
    auto place = [&](int u, int y, int w) { return (size_t(y) * size_t(side) + size_t(w + reach)) * size_t(wide) + size_t(u - uLo); };
    for (int y = 0; y <= yHi; y++)
        for (int u = uLo; u <= uHi; u++) {
            const int fromU = int(std::round(float(u) * c - float(y) * s)), fromY = int(std::round(float(u) * s + float(y) * c));
            if (fromY < 0 || fromY >= height || std::abs(fromU) > reach) continue;
            for (int w = -reach; w <= reach; w++) {
                const int32_t i = grown[cell(fromU, fromY, w)];
                if (i < 0) continue;
                const TreeVoxel& b = t.body[size_t(i)];
                if (!b.trunk && int(hashCell(b.at.x, b.at.y, b.at.z) % 100) < stripped) continue;
                turned[place(u, y, w)] = i;
            }
        }
    // What is joined to the stump, face to face, goes back.
    std::vector<ivec3> open;   // (u, y, w)
    for (int u = uLo; u <= uHi; u++)
        for (int w = -reach; w <= reach; w++)
            if (const int32_t i = turned[place(u, 0, w)]; i >= 0 && t.body[size_t(i)].trunk) {
                joined[place(u, 0, w)] = 1;
                open.push_back(ivec3(u, 0, w));
            }
    std::vector<projv::PendingVoxelOp> adds, removes;
    while (!open.empty()) {
        const ivec3 p = open.back();
        open.pop_back();
        ivec3 at = t.foot;
        at[lean.axis] += lean.sign * p.x;
        at[other] += p.z;
        at.y = p.y;
        if (!grove.inside(at) || grove.filled(at)) continue;   // nothing beyond it is reached through it
        adds.push_back({true, at, t.body[size_t(turned[place(p.x, p.y, p.z)])].colour});
        t.now.push_back(at);
        grove.fill(at, true);
        for (const ivec3 n : {ivec3(1, 0, 0), ivec3(-1, 0, 0), ivec3(0, 1, 0), ivec3(0, -1, 0), ivec3(0, 0, 1), ivec3(0, 0, -1)}) {
            const ivec3 q = p + n;
            if (q.x < uLo || q.x > uHi || q.y < 0 || q.y > yHi || q.z < -reach || q.z > reach) continue;
            const size_t k = place(q.x, q.y, q.z);
            if (turned[k] >= 0 && !joined[k]) {
                joined[k] = 1;
                open.push_back(q);
            }
        }
    }
    // Away goes only what it no longer fills: an edit's additions are made before its removals, so a place
    // both taken away and put back would end up empty.
    for (const ivec3& v : was)
        if (!grove.filled(v)) removes.push_back({false, v, 0u});
    if (grove.body) {
        adds.insert(adds.end(), removes.begin(), removes.end());
        runtime::editVoxels(world, t.e, adds);
        return;
    }
    auto& scene = world.ctx().get<projv::Scene>();
    if (!adds.empty()) utils::queueVoxelAdd(scene, grove.component, adds);
    if (!removes.empty()) utils::queueVoxelRemove(scene, grove.component, removes);
    d.sceneEdited = true;
}

// The gusts due this step, at most GUSTS_PER_STEP (each is an edit of a whole tree, and the front reaches a
// thousand trees a second at its fastest): the rest blow a step or two late.
constexpr int GUSTS_PER_STEP = 70;
void leanTick(World& world, Demo& d) {
    int gusts = 0;
    for (Demo::Lean& lean : d.leaning) {
        if (lean.due > d.boomStep || lean.stage >= LEAN_STAGES) continue;
        if (gusts++ >= GUSTS_PER_STEP) break;
        Target& t = d.targets[lean.target];
        if (world.valid(t.e)) tip(world, d, t, lean);
        lean.stage++;
        lean.due = d.boomStep + LEAN_EVERY;
    }
    std::erase_if(d.leaning, [](const Demo::Lean& lean) { return lean.stage >= LEAN_STAGES; });
    if (d.sceneEdited) utils::updateScene(world.ctx().get<projv::Scene>());   // the scenery groves' edits, at once
    d.sceneEdited = false;
}

// This step's part of the sweep: every target the front has reached since the last, and as many of those
// waiting to be broken as this step's pieces allow -- nearest first, at least one.
void shockSweep(World& world, Demo& d, float radius) {
    const MapSpec& m = *d.map;
    for (size_t i = 0; i < d.targets.size(); i++) {
        Target& t = d.targets[i];
        if (t.struck || !world.valid(t.e)) continue;
        const float distance = distanceTo(d, t);
        if (distance > radius) continue;
        t.struck = true;
        const bool breaks = t.role == Role::Tree ? distance < m.leanRadius : distance < m.wreckRadius;
        if (breaks) d.breaking.push_back({i, distance});
        else if (t.role == Role::Structure && distance < m.glassRadius) blowOutWindows(world, d, t);
    }
    float pieces = 0.0f;
    size_t done = 0;
    for (; done < d.breaking.size(); done++) {
        const auto [i, distance] = d.breaking[done];
        const Target& t = d.targets[i];
        const bool whole = t.role == Role::Tree ? distance < m.shatterRadius : distance < m.wreckRadius;
        // A tree the wind tips over makes no pieces, but its gusts are edits of every voxel of it: a fifth of a
        // piece each.
        float estimate = t.role == Role::Tree && distance >= m.shatterRadius ? 0.2f : 1.0f;
        if (whole) {
            const vec3 cells = (t.hi - t.lo) / shardFor(d, t, distance);
            estimate = std::ceil(cells.x) * std::ceil(cells.z) * (t.storey > 0 ? float(t.voxels.y / t.storey + 1) : std::ceil(cells.y));
        }
        if (done > 0 && pieces + estimate > BREAK_PIECES_PER_STEP) break;
        pieces += estimate;
        if (!world.valid(t.e)) continue;
        if (t.role == Role::Tree) {
            if (distance < m.shatterRadius) strikeWhole(world, d, t, distance);
            else {
                // Away from the blast along whichever of x and z is nearer the way: flat inside fellRadius, and
                // beyond it tipped the further the nearer it stands -- sixty degrees at fellRadius, eighteen at
                // leanRadius.
                const float f = std::clamp((distance - m.fellRadius) / (m.leanRadius - m.fellRadius), 0.0f, 1.0f);
                const bool falls = distance < m.fellRadius;
                const vec3 away = 0.5f * (t.lo + t.hi) - d.blast;
                const int axis = std::abs(away.x) >= std::abs(away.z) ? 0 : 2;
                d.leaning.push_back({i, 0, d.boomStep, glm::radians(falls ? 86.0f : glm::mix(60.0f, 18.0f, f)), axis,
                                     away[axis] >= 0.0f ? 1 : -1, falls, distance < m.stripRadius});
            }
        } else {
            if (distance < m.flattenRadius) strikeWhole(world, d, t, distance);
            else wreck(world, d, t, distance);
        }
    }
    d.breaking.erase(d.breaking.begin(), d.breaking.begin() + std::ptrdiff_t(done));
}

// How many this step, from a rate per second, the fraction carried by chance.
int dueThisStep(Demo& d, float perSecond, float dt) { return int(perSecond * d.emberScale * dt + uniform(d, 0.0f, 1.0f)); }

// The explosion's own steps: the crater and the kicks at once; the fireball bursting out over the first three
// steps; the shockwave's ring; the collar; and the embers that keep coming from the ruins and the ground for a
// while after. Everything is sized by the map's scale: lengths and these embers' speeds by S (they ride the
// cloud's field, scaled alike), what is thrown under gravity by its square root.
void explosionTick(World& world, Demo& d, float dt) {
    if (!d.detonated) return;
    const int step = d.boomStep++;
    const float t = float(step) * dt;
    const float S = d.S, rootS = std::sqrt(d.S);
    if (step == 0) {
        // The crater: a bowl cut out of the earth (fourteen metres in radius in the city), broken into clods
        // that the kick two steps later flings up and out (onBrokenBody makes them boxes).
        if (world.valid(d.earth)) {
            runtime::Strike crater;
            crater.at = vec3(0.0f, -0.1f, 0.0f);
            crater.reach = 14.0f * S;
            crater.shardSize = 3.0f * S;
            crater.push = vec3(0.0f, 22.0f * rootS, 0.0f);
            runtime::fracture(world, d.earth, crater);
        }
        d.kicks.push_back({d.blast, 60.0f * S, 45.0f * rootS, 14.0f * rootS, 2});
        d.kicks.push_back({d.blast + vec3(0, 4.0f * S, 0), 20.0f * S, 8.0f * rootS, 22.0f * rootS, 6});
        addShake(world, 1.0f);
    }
    // The fireball: bursting out all at once, white-hot, fast.
    if (step < 3)
        for (int i = 0; i < int(750.0f * d.emberScale); i++) {
            const vec3 dir = glm::normalize(unitVector(d) + vec3(0.0f, 0.45f, 0.0f));
            spawnEmber(d, d.blast + vec3(0, 2.0f * S, 0) + inBall(d, 5.0f * S), dir * (uniform(d, 22.0f, 70.0f) * S), uniform(d, 0.88f, 1.0f), Kind::Fire);
        }
    // The shockwave's ring of dust, low along the ground.
    if (step >= 1 && step <= 3)
        for (int i = 0; i < int(600.0f * d.emberScale); i++) {
            const float a = uniform(d, 0.0f, 2.0f * PI);
            const vec3 out(std::cos(a), 0.0f, std::sin(a));
            const float u = uniform(d, 0.0f, 1.0f);
            const float height = (0.5f + 8.0f * u * u) * S;
            spawnEmber(d, out * (shockRadius(d, t) + uniform(d, 0.0f, 2.0f * S)) + vec3(0, height, 0), out * shockSpeed(d, t), 0.0f, Kind::Shock);
            Ember& em = d.cloud.back();
            em.out = out;
            em.offset = uniform(d, -6.0f, 3.0f) * S;
            em.height = height;
        }
    // The condensation collar: a flat ring bursting out round the stem, under the cap.
    if (t >= COLLAR_AT && t < COLLAR_AT + 0.35f) {
        mushroom::Field then = d.field;
        then.at(COLLAR_AT);
        const float collarY = 0.55f * then.capHeight;
        int n = dueThisStep(d, 1700.0f, dt);
        while (n-- > 0) {
            const float a = uniform(d, 0.0f, 2.0f * PI);
            const vec3 out(std::cos(a), 0.0f, std::sin(a));
            const float height = collarY + uniform(d, -2.5f, 2.5f) * uniform(d, 0.3f, 1.0f) * S;
            spawnEmber(d, out * collarRadius(d, 0.0f) + vec3(0, height, 0), out * collarSpeed(d, 0.0f), 0.0f, Kind::Collar);
            Ember& em = d.cloud.back();
            em.out = out;
            em.offset = uniform(d, -5.0f, 4.0f) * S;
            em.height = height;
        }
    }
    // The fire in the ruins goes on feeding the stem, fading over five seconds.
    if (t > 0.05f && t < 5.0f) {
        int n = dueThisStep(d, 420.0f * (1.0f - t / 5.0f), dt);
        while (n-- > 0) {
            const float a = uniform(d, 0.0f, 2.0f * PI), r = std::sqrt(uniform(d, 0.0f, 1.0f)) * 8.0f * S;
            const vec3 at = vec3(std::cos(a) * r, uniform(d, 1.0f, 6.0f) * S, std::sin(a) * r);
            spawnEmber(d, at, vec3(0.0f, uniform(d, 9.0f, 20.0f) * S, 0.0f) - at * 0.3f, uniform(d, 0.65f, 0.9f), Kind::Fire);
        }
    }
    // Then the ruins smoulder: dull embers and smoke, keeping the stem fed while the cap climbs.
    if (t > 4.0f && t < 24.0f) {
        int n = dueThisStep(d, 60.0f * (1.0f - (t - 4.0f) / 20.0f), dt);
        while (n-- > 0) {
            const float a = uniform(d, 0.0f, 2.0f * PI), r = std::sqrt(uniform(d, 0.0f, 1.0f)) * 9.0f * S;
            const vec3 at = vec3(std::cos(a) * r, uniform(d, 0.5f, 3.0f) * S, std::sin(a) * r);
            spawnEmber(d, at, vec3(0.0f, uniform(d, 3.0f, 7.0f) * S, 0.0f), uniform(d, 0.22f, 0.4f), Kind::Fire);
        }
    }
    // Dust raised off the ground by the blast, drawn in toward the stem's foot.
    if (t > 0.25f && t < 6.0f) {
        int n = dueThisStep(d, 120.0f * (1.0f - t / 6.0f), dt);
        while (n-- > 0) {
            const float a = uniform(d, 0.0f, 2.0f * PI), r = uniform(d, 14.0f, 50.0f) * S;
            const vec3 out(std::cos(a), 0.0f, std::sin(a));
            spawnEmber(d, out * r + vec3(0.0f, 0.5f * S, 0.0f), (out * uniform(d, 2.0f, 6.0f) + vec3(0, uniform(d, 0.5f, 2.5f), 0)) * S, 0.0f, Kind::Dust);
        }
    }
    // The shock front acts on each target as it reaches it (and the sweep works through those waiting to be
    // broken), shoves what it passes -- debris, cars -- and the camera feels it arrive.
    if (t > 0.0f) shockSweep(world, d, shockRadius(d, t));
    leanTick(world, d);
    if (t > 0.0f && t < 6.0f) {
        const float r0 = shockRadius(d, t - dt), r1 = shockRadius(d, t);
        const float push = std::min(22.0f * rootS, 0.3f * shockSpeed(d, t));
        for (auto [e, rigid, at] : world.view<projv::RigidBody, projv::Transform>().each()) {
            if (rigid.motion != runtime::MotionType::Dynamic || at.position.y > 20.0f * S) continue;
            const vec3 flat(at.position.x - d.blast.x, 0.0f, at.position.z - d.blast.z);
            const float r = glm::length(flat);
            if (r < r0 || r >= r1 || r < 1e-3f || !runtime::hasBody(world, e)) continue;
            runtime::addVelocity(world, e, flat / r * push + vec3(0.0f, 0.25f * push, 0.0f));
        }
        const vec3 eye = world.ctx().get<Camera>().position;
        const float eyeDistance = glm::length(vec3(eye.x - d.blast.x, 0.0f, eye.z - d.blast.z));
        if (eyeDistance >= r0 && eyeDistance < r1) addShake(world, 0.7f);
    }
}

void kickTick(World& world, Demo& d) {
    for (Kick& k : d.kicks) {
        if (--k.steps != 0) continue;
        for (Entity e : runtime::overlapSphere(world, k.at, k.radius, runtime::layerBit(runtime::PhysicsLayer::Moving))) {
            if (!world.all_of<projv::RigidBody>(e) || world.get<projv::RigidBody>(e).motion != runtime::MotionType::Dynamic) continue;
            const vec3 offset = runtime::bodyPose(world, e).centerOfMass - k.at;
            const float distance = glm::length(offset);
            if (distance < 1e-3f) continue;
            const float falloff = 1.0f - std::min(distance / k.radius, 1.0f);
            const float jitter = 0.75f + 0.5f * float(hashCell(int(e), 1, 2) % 100) / 100.0f;
            runtime::addVelocity(world, e, (offset / distance * k.speed + vec3(0, k.lift, 0)) * (falloff * jitter));
        }
    }
    std::erase_if(d.kicks, [](const Kick& k) { return k.steps <= 0; });
}

// Each ember is dragged toward where it should be going -- its ring, while it rides one, or the field plus its
// own buoyancy -- moves on, stays above the ground, cools, and goes at the end of its life.
void cloudTick(Demo& d, float dt) {
    if (!d.detonated) return;
    const float S = d.S;
    d.field.at(float(d.boomStep) * dt);
    for (Ember& em : d.cloud) {
        em.age += dt;
        em.temperature *= std::exp(-em.cooling * dt);
        if (em.age > em.lifetime) {
            em.dead = true;
            continue;
        }
        const vec3 p = em.position;
        vec3 flow;
        float tau;
        if (em.kind == Kind::Shock && em.age < SHOCK_HOLD) {
            // Riding the shock ring: out at its speed, held to its line, thickening as it slows.
            const float t = em.born + em.age;
            const float radius = shockRadius(d, t) + em.offset * (1.0f + 0.6f * t);
            const vec3 onRing = d.blast * vec3(1, 0, 1) + em.out * radius + vec3(0.0f, em.height * (1.0f + 0.45f * t), 0.0f);
            flow = em.out * shockSpeed(d, t) + (onRing - p) * 3.0f + mushroom::curlNoise(p, 0.3f / S, 0.8f * t * S, t, 2.0f);
            tau = 0.1f;
        } else if (em.kind == Kind::Collar && em.age < COLLAR_HOLD) {
            // Riding the collar: out round the stem, rising slowly with the cap.
            const float radius = collarRadius(d, em.age) + em.offset * (1.0f + 0.4f * em.age);
            const vec3 onRing = d.blast * vec3(1, 0, 1) + em.out * radius + vec3(0.0f, em.height + 1.5f * S * em.age, 0.0f);
            flow = em.out * collarSpeed(d, em.age) + (onRing - p) * 3.0f + mushroom::curlNoise(p, 0.35f / S, 0.6f * S, em.age, 4.0f);
            tau = 0.12f;
        } else {
            flow = d.field.velocity(p) + vec3(0.0f, 10.0f * S * em.temperature, 0.0f);
            // The burst flies out loose for its first moments before the field takes it; then it follows.
            tau = em.kind == Kind::Fire ? (em.age < 0.25f ? 0.9f : em.age < 0.8f ? 0.35f : 0.3f) : 0.6f;
        }
        em.velocity += (flow - em.velocity) * (1.0f - std::exp(-dt / tau));
        em.previous = em.position;
        em.position += em.velocity * dt;
        if (em.position.y < 0.4f * S) {
            em.position.y = 0.4f * S;
            em.velocity.y = std::max(em.velocity.y, 0.0f);
        }
    }
    std::erase_if(d.cloud, [](const Ember& em) { return em.dead; });
}

// A piece's voxel count, for the budget.
void notePieceSize(World& world, Entity piece) {
    const projv::Scene* scene = world.ctx().find<projv::Scene>();
    const projv::VoxelComponent* link = world.try_get<projv::VoxelComponent>(piece);
    if (!scene || !link) return;
    world.emplace_or_replace<PieceSize>(piece, PieceSize{utils::getComponentVoxelCount(*scene, link->handle)});
}

// What breaks off a target is thrown out from ground zero through where each piece is, at blastSpeed for that
// distance (jittered), lifted a little and spinning; what falls because its support went, at half that. Nearest
// ground zero some of it is vaporised outright (the fireball hides where it went). The pieces collide as boxes
// and do not break again where they land: thousands of voxel-shaped bodies breaking at every bounce would be
// most of the physics. A big piece that fell for want of support is broken up as it comes down.
void throwPieces(World& world, Demo& d, const Target& t, const projv::BodyBroken& e) {
    const bool fallen = e.cause != projv::BreakCause::Strike;
    for (size_t i = 0; i < e.pieces.size(); i++) {
        const Entity piece = e.pieces[i];
        if (!world.valid(piece)) continue;
        const std::vector<ivec3>* voxels = i < e.pieceVoxels.size() && !e.pieceVoxels[i].empty() ? &e.pieceVoxels[i] : nullptr;
        vec3 at = e.at;
        if (voxels) {
            vec3 sum{0.0f};
            for (const ivec3& v : *voxels) sum += vec3(v);
            at = t.worldOf(sum / float(voxels->size()));
        }
        const float distance = glm::length(at - d.blast);
        const uint32_t h = hashCell(int(piece), int(i), 17);
        if (float(h % 1000) / 1000.0f < 0.6f * std::clamp(1.0f - distance / (35.0f * d.S), 0.0f, 1.0f)) {
            world.destroy(piece);
            continue;
        }
        if (projv::RigidBody* body = world.try_get<projv::RigidBody>(piece)) {
            body->shape = projv::RigidBody::Shape::Box;
            body->breaksOnImpact = false;
        }
        // The power is cut: its lit windows go dark.
        if (const ComponentHandle c = runtime::voxelComponentOf(world, piece); c != projv::INVALID_COMPONENT_HANDLE)
            glow(world.ctx().get<projv::Scene>(), c, 0.0f);
        const float speed = blastSpeed(d, distance);
        if (fallen && voxels && voxels->size() > 200) {
            ivec3 lo = voxels->front(), hi = voxels->front();
            for (const ivec3& v : *voxels) { lo = glm::min(lo, v); hi = glm::max(hi, v); }
            runtime::Strike strike;
            strike.at = t.worldOf(vec3((*voxels)[voxels->size() / 2]));
            strike.reach = glm::length(vec3(hi - lo + 1)) * t.voxel + 1.0f;
            strike.shardSize = flattenShard(d, distance);
            runtime::fracture(world, piece, strike);
        }
        vec3 out = at - d.blast;
        out.y = std::max(out.y * 0.5f, 0.0f);
        out = glm::length(out) > 1e-3f ? glm::normalize(out) : vec3(0.0f, 1.0f, 0.0f);
        const float thrown = speed * (fallen ? 0.5f : 1.0f);
        const float jitter = 0.7f + 0.6f * float((h >> 3) % 1000) / 1000.0f;
        const vec3 wobble = vec3(float((h >> 4) % 100), float((h >> 11) % 100), float((h >> 18) % 100)) / 50.0f - 1.0f;
        runtime::setVelocity(world, piece, (out + wobble * 0.25f) * (thrown * jitter) + vec3(0.0f, 0.12f * thrown, 0.0f),
                             wobble * (0.08f * thrown / std::sqrt(d.S)));
    }
}

void onBrokenBody(World& world, const projv::BodyBroken& e) {
    auto& d = world.ctx().get<Demo>();
    for (Entity piece : e.pieces)
        if (world.valid(piece)) notePieceSize(world, piece);
    if (world.valid(e.entity) && world.all_of<projv::Piece>(e.entity)) notePieceSize(world, e.entity);   // what is left of it
    d.pieces += int(e.pieces.size());
    // The crater's clods collide as boxes fitted to their voxels: a pile of voxel-shaped clods in the bowl
    // cost the physics four times what the rest of the scene did.
    if (e.entity == d.earth)
        for (Entity piece : e.pieces)
            if (world.valid(piece))
                if (projv::RigidBody* body = world.try_get<projv::RigidBody>(piece)) body->shape = projv::RigidBody::Shape::Box;
    for (const Target& t : d.targets)
        if (t.e == e.entity) {
            throwPieces(world, d, t, e);
            break;
        }
    // A strike's shards pass through each other for their first half second (as in Voxel Mayhem: they are born
    // flush against each other, and the solver would spend the break's first steps on them).
    if (e.shards <= 1) return;
    static uint32_t nextFamily = 1000;
    const uint32_t family = nextFamily++;
    for (size_t i = 0; i < e.shards; i++)
        if (world.valid(e.pieces[i])) {
            projv::CollisionGroups g = world.all_of<projv::CollisionGroups>(e.pieces[i]) ? world.get<projv::CollisionGroups>(e.pieces[i])
                                                                                          : projv::CollisionGroups{};
            g.family = family;
            world.emplace_or_replace<projv::CollisionGroups>(e.pieces[i], g);
            d.newborn.push_back({e.pieces[i], SHARD_FAMILY_STEPS});
        }
}

// Around "physics": the whole system's time, step by step.
void stepStart(projv::Application& app) { app.world.ctx().get<Demo>().stepStart = Clock::now(); }
void stepEnd(projv::Application& app) {
    auto& d = app.world.ctx().get<Demo>();
    const double ms = msSince(d.stepStart);
    d.physicsSecondMs += ms;
    d.physicsLastMs = ms;
    d.physicsWorstMs = std::max(d.physicsWorstMs, ms);
    d.stepsThisSecond++;
}

// The piece budget: past pieceBudget pieces, the smallest go -- oldest first among the same size -- at most
// CULL_PER_STEP a step (destroying a piece costs the step as making one does, and the breaking makes them in
// bursts). A big piece goes only when nothing smaller is left to clear.
constexpr size_t CULL_PER_STEP = 120;
void cullPieces(World& world, Demo& d) {
    auto pieces = world.view<projv::Piece>();
    size_t count = 0;
    for (Entity e : pieces) { (void)e; count++; }
    if (count <= d.pieceBudget) return;
    struct Candidate { uint32_t voxels; uint64_t madeAt; Entity e; };
    std::vector<Candidate> all;
    all.reserve(count);
    for (Entity e : pieces) {
        const PieceSize* size = world.try_get<PieceSize>(e);
        all.push_back({size ? size->voxels : 0u, world.get<projv::Piece>(e).madeAt, e});
    }
    const size_t excess = std::min(count - d.pieceBudget, CULL_PER_STEP);
    std::nth_element(all.begin(), all.begin() + std::ptrdiff_t(excess), all.end(), [](const Candidate& a, const Candidate& b) {
        return a.voxels != b.voxels ? a.voxels < b.voxels : a.madeAt < b.madeAt;
    });
    for (size_t i = 0; i < excess; i++)
        if (world.valid(all[i].e)) world.destroy(all[i].e);
}

// Debris that has come to rest stops being simulated: once a piece has lain still for SETTLE_SECONDS its body
// goes (the RigidBody comes off) and it stays where it lies, drawn and nothing more. Thousands of resting
// pieces, all awake against the voxel ground and each other, were most of the physics step after the blast.
// What lands on a settled piece later passes through it; the pile is too chaotic to tell. Looked at every
// SETTLE_EVERY steps.
constexpr float SETTLE_SECONDS = 0.6f, SETTLE_SPEED = 1.2f;
constexpr int SETTLE_EVERY = 6;
void settleTick(World& world, Demo& d, float dt) {
    if (!d.settle || !d.detonated || d.boomStep % SETTLE_EVERY != 0) return;
    std::vector<Entity> settled;
    for (auto [e, piece, rigid] : world.view<projv::Piece, projv::RigidBody>().each()) {
        if (!runtime::hasBody(world, e)) continue;
        const runtime::BodyState state = runtime::bodyPose(world, e);
        Stillness& still = world.get_or_emplace<Stillness>(e);
        const bool slow = glm::length(state.linearVelocity) < SETTLE_SPEED && glm::length(state.angularVelocity) < SETTLE_SPEED;
        still.seconds = slow ? still.seconds + float(SETTLE_EVERY) * dt : 0.0f;
        if (still.seconds >= SETTLE_SECONDS) settled.push_back(e);
    }
    for (Entity e : settled) world.remove<projv::RigidBody>(e);
}

// Shards out of their break's family once its steps are up.
void newbornTick(World& world, Demo& d) {
    for (auto& [piece, steps] : d.newborn)
        if (--steps <= 0 && world.valid(piece))
            if (projv::CollisionGroups* g = world.try_get<projv::CollisionGroups>(piece)) {
                g->family = 0;
                world.patch<projv::CollisionGroups>(piece);
            }
    std::erase_if(d.newborn, [&](const auto& n) { return n.second <= 0 || !world.valid(n.first); });
}

void fixedTick(projv::Application& app) {
    World& world = app.world;
    auto& d = world.ctx().get<Demo>();
    const float dt = app.time().fixedDelta;
    const auto start = Clock::now();
    explosionTick(world, d, dt);
    kickTick(world, d);
    newbornTick(world, d);
    settleTick(world, d, dt);
    cullPieces(world, d);
    cloudTick(d, dt);
    d.fixedMs = msSince(start);
}

// =================================================================================================
// Presenting the cloud: the ember chunks where they are, the fire's light, the smoke grid
// =================================================================================================

// A fire ember's colour and glow at a temperature: STAGES, blended between its entries.
void fireLook(float temperature, vec3& colour, float& glow) {
    for (int k = 0; k + 1 < STAGE_COUNT; k++) {
        const Stage& hot = STAGES[k];
        const Stage& cool = STAGES[k + 1];
        if (temperature >= hot.above || k + 2 == STAGE_COUNT) {
            const float span = hot.above - std::max(cool.above, 0.0f);
            const float f = k == 0 && temperature >= hot.above ? 1.0f : std::clamp((temperature - std::max(cool.above, 0.0f)) / span, 0.0f, 1.0f);
            colour = glm::mix(cool.colour, hot.colour, f);
            glow = glm::mix(cool.glow, hot.glow, f);
            return;
        }
    }
    colour = STAGES[STAGE_COUNT - 1].colour;
    glow = 0.0f;
}

// Three shades, darkest first, blended by how far along it is (0..1).
vec3 shadeOf(const vec3 (&shades)[PUFF_SHADES], float along) {
    const float f = std::clamp(along, 0.0f, 1.0f) * float(PUFF_SHADES - 1);
    const int i = std::min(int(f), PUFF_SHADES - 2);
    return glm::mix(shades[i], shades[i + 1], f - float(i));
}

void presentCloud(projv::Application& app) {
    World& world = app.world;
    auto& d = world.ctx().get<Demo>();
    Look& look = world.ctx().get<Look>();
    const auto start = Clock::now();
    const float S = d.S;
    float weights = 0.0f, heat = 0.0f;
    vec3 fireAt{0.0f};
    d.splats.clear();
    d.cubes.clear();
    const float alpha = app.time().fixedAlpha;   // drawn between its last two steps, as bodies are
    for (Ember& em : d.cloud) {
        const vec3 position = glm::mix(em.previous, em.position, alpha);
        const float T = em.temperature;

        // Its cubes. Fire grows a little as the gas expands and glows by its temperature; the rest show their
        // own colour. With a smoke pass they shrink away as they cool (the pass draws the smoke); with none --
        // pure voxels -- they become the smoke themselves: darkening to soot, swelling and drifting, and
        // shrinking away only at the end of their life.
        const bool fire = em.kind == Kind::Fire;
        const bool pureVoxels = look.smokeMode == 0;
        float scale;
        vec3 colour;
        float glowing = 0.0f;
        const float active = fire ? 0.0f : em.chunkLife * 0.5f;   // how long the others show their own colour
        const bool puffing = pureVoxels && (fire ? T <= STAGES[STAGE_COUNT - 2].above : em.age > active);
        if (puffing) {
            if (em.smokeSince < 0.0f) em.smokeSince = em.age;
            const float since = em.age - em.smokeSince;
            colour = fire ? shadeOf(SOOT, since / 12.0f) : em.kind == Kind::Collar ? shadeOf(VAPOUR_PUFF, since / 4.0f) : shadeOf(DUST_PUFF, since / 12.0f);
            if (fire) glowing = 0.5f * std::max(0.0f, 1.0f - since / 1.5f);   // the last of the glow
            const float swell = fire ? 1.2f + 1.4f * mushroom::smoothstep(0.0f, 16.0f, since)
                                     : 1.0f + 0.6f * mushroom::smoothstep(0.0f, 5.0f, since);
            scale = swell * (1.0f - mushroom::smoothstep(em.lifetime - 8.0f, em.lifetime, em.age));
        } else if (fire) {
            fireLook(T, colour, glowing);
            scale = (0.6f + 0.6f * mushroom::smoothstep(0.0f, 1.5f, em.age)) * (pureVoxels ? 1.0f : mushroom::smoothstep(0.03f, 0.2f, T));
        } else {
            const float size = em.kind == Kind::Shock ? 1.0f : em.kind == Kind::Collar ? 0.9f : 0.8f;
            colour = em.kind == Kind::Shock ? vec3(0.6f, 0.5f, 0.38f) : em.kind == Kind::Collar ? vec3(0.84f, 0.87f, 0.92f) : vec3(0.28f, 0.23f, 0.18f);
            glowing = em.kind == Kind::Shock ? 0.9f : em.kind == Kind::Collar ? 0.7f : 0.0f;
            scale = size * (pureVoxels ? 1.0f : 1.0f - mushroom::smoothstep(em.chunkLife * 0.45f, em.chunkLife, em.age));
        }
        em.scale = scale;
        if (d.chunks && scale >= 0.03f) {
            const quat turn = glm::angleAxis(em.age * em.spin, em.axis);
            for (int i = 0; i < em.cubeCount; i++) {
                const vec4 c = em.cubes[size_t(i)];
                const vec3 tint = (i % 2) ? colour : colour * 0.84f;
                d.cubes.push_back({position + turn * (vec3(c) * scale), c.w * scale, turn, tint, glowing, em.seeds[size_t(i)]});
            }
        }

        // Its smoke: a little while it burns, all of it once it has cooled, spreading as it ages and fading out
        // at the end of its life. The collar's vapour evaporates within seconds.
        const float fade = 1.0f - mushroom::smoothstep(em.lifetime - (em.kind == Kind::Collar ? 7.0f : 14.0f), em.lifetime, em.age);
        float mass = em.smoke * fade, sigma = 0.9f + 0.22f * em.age + 1.8f * (1.0f - T);
        if (fire) mass *= 0.3f + 0.7f * mushroom::smoothstep(0.65f, 0.08f, T);
        if (em.kind == Kind::Shock) sigma = 1.0f + 0.5f * em.age;
        if (em.kind == Kind::Collar) sigma = 0.9f + 0.45f * em.age;
        d.splats.push_back({position, sigma * S, mass, T, em.kind == Kind::Dust || em.kind == Kind::Shock ? 1.0f : 0.0f,
                            em.kind == Kind::Collar ? 1.0f : 0.0f});
        if (!fire) continue;

        // The fire's light comes from where the hot embers are, weighted by how hot.
        const float w = T * T * T * T;
        weights += w;
        fireAt += position * w;
        heat += T * w;
    }
    night_renderer::uploadEmbers(world, d.cubes);
    d.presentMs = msSince(start);

    // The explosion's light: a blinding flash in the first instant, then the fireball's glow, cooling. On a
    // smaller map everything it lights is nearer, so it is dimmer by the square of the scale.
    const float t = d.detonated ? float(d.boomStep) * app.time().fixedDelta : -1.0f;
    const float burst = t >= 0.0f ? 900000.0f * S * S * std::exp(-t / 0.3f) : 0.0f;
    const float glowLevel = weights * 160.0f * S * S;
    const vec3 middle = d.blast + vec3(0.0f, 0.4f * S, 0.0f);
    look.flash = burst + glowLevel;
    look.flashAt = weights > 1e-4f ? fireAt / weights : middle;
    if (burst > glowLevel) look.flashAt = glm::mix(look.flashAt, middle, std::min(1.0f, burst / (glowLevel + 1.0f)));
    const float meanHeat = weights > 1e-4f ? heat / weights : 1.0f;
    look.flashColour = glm::mix(vec3(1.0f, 0.36f, 0.08f), vec3(1.0f, 0.72f, 0.4f), mushroom::smoothstep(0.4f, 1.0f, meanHeat));
    look.flashRadius = 3.0f * S + 0.4f * d.field.ringRadius;
    if (look.flash < 0.5f) look.flash = 0.0f;
    // The eye: the exposure drops while the fire is bright, and comes back slowly as it fades -- in the
    // simulation's time, so slow motion and a recording see the same as real time.
    const float wanted = d.baseExposure / (1.0f + look.flash / (S * S) * 0.000005f);
    const float rate = wanted < look.exposure ? 12.0f : 0.8f;
    look.exposure += (wanted - look.exposure) * (1.0f - std::exp(-rate * app.time().delta));
}

// The smoke grid: splatted on worker threads and uploaded, every frame.
void buildCloudGrid(projv::Application& app) {
    World& world = app.world;
    auto& d = world.ctx().get<Demo>();
    Look& look = world.ctx().get<Look>();
    look.seconds = d.clock;
    if (look.smokeMode == 0) {   // pure voxels: no grid to build
        d.cloudMs = 0.0;
        return;
    }
    const auto start = Clock::now();
    d.grid.min = look.smokeMin;
    d.grid.cell = look.smokeCell;
    d.grid.build(d.splats, 12);
    d.grid.pack(d.atlas, night_renderer::TILES);
    night_renderer::uploadSmoke(world, d.atlas);
    d.cloudMs = msSince(start);
}

// =================================================================================================
// Camera and controls
// =================================================================================================

void printHelp() {
    std::printf(
        "\n  EXPLOSION -- a city, a house or a forest, blown apart\n\n"
        "  Space   detonate (it goes off by itself 4 s after a build)     R  rebuild   1 2 3  city, house, forest\n"
        "  F       fly (mouse, W/A/S/D, Space/Shift, Ctrl fast; Esc frees the cursor) / orbit\n"
        "  Left/Right, Up/Down   turn the orbit, nearer / farther         T  slow motion   P  pause\n"
        "  M  smoke: pure voxels / voxel blocks / soft gas   E  ember chunks   H  shadows   I  GI   K  TAA\n"
        "  N  day / night   [ ]  exposure   F1  this help\n\n");
}

void rebuild(World& world, Demo& d, const MapSpec& map) {
    clearWorld(world);
    buildWorld(world, map);
    d.detonateAt = d.clock + 4.0f;
}

void controls(projv::Application& app) {
    World& world = app.world;
    auto& renderInstance = world.ctx().get<projv::graphics::RenderInstance>();
    const auto& input = world.ctx().get<projv::Input>();
    auto& camera = world.ctx().get<Camera>();
    auto& d = world.ctx().get<Demo>();
    Look& look = world.ctx().get<Look>();
    projv::Time& time = app.time();
    const float dt = time.unscaledDelta;
    d.clock += time.delta;

    if (!d.detonated && d.detonateAt >= 0.0f && d.clock >= d.detonateAt) detonate(d);

    if (!d.unattended) {
        if (input.pressed(projv::Key::F1)) printHelp();
        if (input.pressed(projv::Key::F)) {
            camera.fly = !camera.fly;
            projv::graphics::setCursorCaptured(app, renderInstance, camera.fly);
        }
        if (input.cursorCaptured && input.pressed(projv::Key::Escape)) projv::graphics::setCursorCaptured(app, renderInstance, false);
        else if (camera.fly && !input.cursorCaptured && input.pressed(projv::MouseButton::Left))
            projv::graphics::setCursorCaptured(app, renderInstance, true);
        if (input.pressed(projv::Key::R)) rebuild(world, d, *d.map);
        if (input.pressed(projv::Key::Num1)) rebuild(world, d, MAPS[0]);
        if (input.pressed(projv::Key::Num2)) rebuild(world, d, MAPS[1]);
        if (input.pressed(projv::Key::Num3)) rebuild(world, d, MAPS[2]);
        if (input.pressed(projv::Key::Space) && !camera.fly) detonate(d);
        if (input.pressed(projv::Key::Enter)) detonate(d);
        // T: slow motion, deeper each press -- a quarter, a tenth, a thirtieth -- then back to real time.
        if (input.pressed(projv::Key::T)) time.scale = time.scale > 0.3f ? 0.25f : time.scale > 0.07f ? 0.1f : time.scale > 0.02f ? 1.0f / 30.0f : 1.0f;
        if (input.pressed(projv::Key::P)) time.scale = time.scale == 0.0f ? 1.0f : 0.0f;
        if (input.pressed(projv::Key::M)) {
            look.smokeMode = (look.smokeMode + 1) % 3;
            static const char* names[3] = {"pure voxels", "voxel blocks", "soft gas"};
            std::printf("smoke: %s\n", names[look.smokeMode]);
        }
        if (input.pressed(projv::Key::E)) d.chunks = !d.chunks;   // the ember cubes
        if (input.pressed(projv::Key::H)) look.shadows = !look.shadows;
        if (input.pressed(projv::Key::I)) look.rayGI = !look.rayGI;
        if (input.pressed(projv::Key::K)) look.taa = !look.taa;
        if (input.pressed(projv::Key::N)) {
            look.day = !look.day;
            d.baseExposure = look.day ? Look::dayExposure : Look::nightExposure;
        }
        if (input.pressed(projv::Key::LeftBracket)) d.baseExposure /= 1.25f;
        if (input.pressed(projv::Key::RightBracket)) d.baseExposure *= 1.25f;
    }
    if (d.fixedCamera) return;

    const float S = d.S;
    if (camera.fly) {
        if (input.cursorCaptured) {
            camera.yaw += input.cursorDelta.x * 0.0025f;
            camera.pitch = std::clamp(camera.pitch - input.cursorDelta.y * 0.0025f, -1.5f, 1.5f);
        }
        const vec3 forward = camera.forward(), right = glm::normalize(glm::cross(forward, vec3(0, 1, 0)));
        vec3 move{0.0f};
        if (input.down(projv::Key::W)) move += forward;
        if (input.down(projv::Key::S)) move -= forward;
        if (input.down(projv::Key::D)) move += right;
        if (input.down(projv::Key::A)) move -= right;
        if (input.down(projv::Key::Space)) move.y += 1.0f;
        if (input.down(projv::Key::LeftShift)) move.y -= 1.0f;
        camera.position += move * ((input.down(projv::Key::LeftControl) ? 120.0f : 40.0f) * S * dt);
        return;
    }
    // The orbit: slowly round ground zero, rising and backing off to keep the cloud in frame.
    if (!d.unattended) {
        if (input.down(projv::Key::Left)) camera.orbit -= 0.8f * dt;
        if (input.down(projv::Key::Right)) camera.orbit += 0.8f * dt;
        if (input.down(projv::Key::Up)) camera.distance = std::max(20.0f * S, camera.distance - 60.0f * S * dt);
        if (input.down(projv::Key::Down)) camera.distance = std::min(500.0f * S, camera.distance + 60.0f * S * dt);
    }
    camera.orbit += 0.03f * dt;
    const float cap = d.detonated ? std::min(d.field.capHeight, 140.0f * S) : 0.0f;
    const vec3 wantTarget(0.0f, d.map->targetHeight + 0.5f * cap, 0.0f);
    camera.target += (wantTarget - camera.target) * (1.0f - std::exp(-1.5f * dt));
    const float distance = camera.distance + 0.55f * cap;
    camera.position = vec3(std::cos(camera.orbit) * distance, d.map->orbitHeight + 0.12f * cap, std::sin(camera.orbit) * distance);
    const vec3 look3 = glm::normalize(camera.target - camera.position);
    camera.yaw = std::atan2(look3.z, look3.x);
    camera.pitch = std::asin(std::clamp(look3.y, -1.0f, 1.0f));
}

void titleBar(projv::Application& app) {
    World& world = app.world;
    auto& d = world.ctx().get<Demo>();
    d.framesThisSecond++;
    const auto& stats = runtime::physicsWorld(world).stats();
    if ((d.titleTimer -= app.time().unscaledDelta) <= 0.0f) {
        d.titleTimer = 0.25f;
        const std::string title = fmt::format(
            "Explosion: {}  |  {}  |  {} bodies, {} embers, {} pieces  |  physics {:.1f} ms, cloud {:.1f} ms  |  {} fps{}",
            d.map->name, d.detonated ? fmt::format("t+{:.1f} s", float(d.boomStep) * app.time().fixedDelta) : "armed",
            stats.bodies, d.cloud.size(), d.pieces, stats.lastStepMilliseconds, d.cloudMs + d.presentMs,
            int(1.0f / std::max(app.time().unscaledDelta, 1e-4f)),
            app.time().scale == 0.0f ? ", PAUSED" : app.time().scale < 1.0f ? ", slow motion" : "");
        glfwSetWindowTitle(world.ctx().get<projv::graphics::RenderInstance>().window, title.c_str());
    }
    if (d.stats && d.detonated && d.boomStep < 120)
        std::printf("  boom step %d: frame %.1f ms, physics system %.1f ms (step %.1f), embers %.1f, present %.1f, flush %.1f\n", d.boomStep,
                    d.frameMs, d.physicsLastMs, stats.lastStepMilliseconds, d.fixedMs, d.presentMs, d.flushMs);
    if (d.stats && (d.statsTimer -= app.time().unscaledDelta) <= 0.0f) {
        d.statsTimer += 1.0f;
        std::printf("explosion: t=%.1f fps=%d bodies=%u awake=%u embers=%zu cubes=%zu pieces=%d | frame %.1f ms: physics %.1f, embers %.1f, "
                    "present %.1f, grid %.1f, flush %.1f, render %.1f | cap %.0f m\n",
                    d.clock, d.framesThisSecond, unsigned(stats.bodies), unsigned(stats.awakeBodies), d.cloud.size(), d.cubes.size(), d.pieces, d.frameMs,
                    stats.lastStepMilliseconds, d.fixedMs, d.presentMs, d.cloudMs, d.flushMs, d.renderMs, d.field.capHeight);
        std::printf("    physics system: %d steps, %.0f ms in all, worst step %.1f ms\n", d.stepsThisSecond, d.physicsSecondMs, d.physicsWorstMs);
        std::fflush(stdout);
        d.stepsThisSecond = 0;
        d.physicsSecondMs = d.physicsWorstMs = 0.0;
        d.framesThisSecond = 0;
    }
}

// Unattended runs: screenshots at the asked-for times, or every frame while recording, then quit.
void unattendedTick(projv::Application& app) {
    auto& d = app.world.ctx().get<Demo>();
    if (d.recordFps > 0.0f && d.clock >= d.recordFrom) {
        const std::string name = fmt::format("{}_{:05d}", d.capturePrefix, d.recorded++);
        bgfx::requestScreenShot(BGFX_INVALID_HANDLE, name.c_str());
    }
    static size_t nextShot = 0;
    if (nextShot < d.shots.size() && d.clock >= d.shots[nextShot]) {
        const std::string name = fmt::format("{}_{:05.1f}", d.capturePrefix, d.shots[nextShot]);
        bgfx::requestScreenShot(BGFX_INVALID_HANDLE, name.c_str());
        nextShot++;
    }
    if (d.exitAt > 0.0f && d.clock >= d.exitAt) app.closeRequested = true;
}

// =================================================================================================
// Startup, render, shutdown
// =================================================================================================

std::vector<float> numbers(const char* text) {
    std::vector<float> out;
    for (const char* p = text; p && *p;) {
        char* end = nullptr;
        const float v = std::strtof(p, &end);
        if (end == p) break;
        out.push_back(v);
        p = *end == ',' ? end + 1 : end;
    }
    return out;
}

void startup(projv::Application& app) {
    World& world = app.world;
    // The engine logs every edit and upload at info level; with thousands of pieces coming and going that is
    // thousands of lines a second. Warnings and errors only, unless asked (EXPLOSION_VERBOSE).
    if (!std::getenv("EXPLOSION_VERBOSE")) spdlog::set_level(spdlog::level::warn);
    const fs::path here = projv::core::executableDirectory();

    auto& d = world.ctx().emplace<Demo>();
    auto& camera = world.ctx().emplace<Camera>();
    if (const char* v = std::getenv("EXPLOSION_LOOK")) {
        const std::vector<float> n = numbers(v);
        if (n.size() == 6) {
            d.fixedCamera = true;
            camera.position = vec3(n[0], n[1], n[2]);
            const vec3 dir = glm::normalize(vec3(n[3], n[4], n[5]) - camera.position);
            camera.yaw = std::atan2(dir.z, dir.x);
            camera.pitch = std::asin(dir.y);
        }
    }
    if (const char* v = std::getenv("EXPLOSION_SHOTS")) d.shots = numbers(v);
    if (const char* v = std::getenv("EXPLOSION_CAPTURE")) d.capturePrefix = v;
    if (const char* v = std::getenv("EXPLOSION_EXIT")) d.exitAt = std::strtof(v, nullptr);
    if (const char* v = std::getenv("EXPLOSION_DETONATE")) d.detonateAt = std::strtof(v, nullptr);
    d.stats = std::getenv("EXPLOSION_STATS") != nullptr;
    d.unattended = d.exitAt > 0.0f;
    if (const char* v = std::getenv("EXPLOSION_EMBERS")) d.emberScale = std::clamp(std::strtof(v, nullptr), 0.1f, 10.0f);
    // EXPLOSION_TIMESCALE: slow motion from the start (0.05 is a twentieth). The simulation then takes as long
    // a step as it needs: the frames wait on it, not the other way round.
    if (const char* v = std::getenv("EXPLOSION_TIMESCALE")) d.timeScale = std::clamp(std::strtof(v, nullptr), 0.005f, 1.0f);
    // EXPLOSION_RECORD: every frame is one 1/fps step of the simulation however long it takes -- Time clamps a
    // frame to maxDelta, and the scale makes that clamped frame 1/fps -- so nothing is dropped and the frames,
    // played at fps, are real time. Fine steps and the big piece budget, as in slow motion.
    if (const char* v = std::getenv("EXPLOSION_RECORD")) {
        d.recordFps = std::clamp(std::strtof(v, nullptr), 10.0f, 120.0f);
        d.timeScale = 0.5f;
        d.recordFrom = std::max(0.0f, d.detonateAt - 0.5f);
        if (const char* from = std::getenv("EXPLOSION_RECORD_FROM")) d.recordFrom = std::strtof(from, nullptr);
    }

    auto& renderInstance = world.ctx().emplace<projv::graphics::RenderInstance>();
    // An unattended run's window may not be resized (a tiling window manager would otherwise give it whatever
    // shape the screen has room for, and change it mid-run): 1600 x 900, a recording 1920 x 1080, or
    // EXPLOSION_SIZE=WxH.
    int width = 1600, height = 900;
    if (d.recordFps > 0.0f) width = 1920, height = 1080;
    if (const char* v = std::getenv("EXPLOSION_SIZE")) std::sscanf(v, "%dx%d", &width, &height);
    renderInstance.initialize(width, height, "Explosion");
    if (d.unattended) {
        glfwSetWindowAttrib(renderInstance.window, GLFW_RESIZABLE, GLFW_FALSE);
        glfwSetWindowSize(renderInstance.window, width, height);
    }
    projv::graphics::installPlatform(app, renderInstance);

    world.ctx().emplace<Shake>();
    Look& look = world.ctx().emplace<Look>();
    if (const char* v = std::getenv("EXPLOSION_NIGHT")) look.day = std::atoi(v) == 0;
    look.exposure = look.day ? Look::dayExposure : Look::nightExposure;
    if (const char* v = std::getenv("EXPLOSION_EXPOSURE")) look.exposure = std::max(0.01f, std::strtof(v, nullptr));
    d.baseExposure = look.exposure;
    // --graphics: what the renderer does beyond the primary ray (H, I and K still toggle by hand).
    look.shadows = graphicsLevel >= 2;
    look.ambientOcclusion = true;
    look.bounce = graphicsLevel >= 3 ? 1 : 0;   // the colour bounce (worked out in the SSAO pass)
    look.rayGI = graphicsLevel >= 3;
    if (const char* v = std::getenv("EXPLOSION_SMOKE")) look.smokeMode = std::clamp(std::atoi(v), 0, 2);
    auto& scene = world.ctx().emplace<projv::Scene>();

    runtime::installSceneBridge(app);
    runtime::PhysicsConfig physics;
    physics.settings.worldMin = vec3(-400.0f, -40.0f, -400.0f);
    physics.settings.worldMax = vec3(400.0f, 500.0f, 400.0f);
    if (const char* v = std::getenv("EXPLOSION_THREADS")) physics.settings.threads = std::atoi(v);   // the step's workers (-1: up to 7)
    // Real time affords 2,500 pieces; slow motion, thirty thousand. The engine's own budget clears the oldest
    // whatever their size; this one is ours (cullPieces), and clears the smallest first.
    physics.maxPieces = 0;
    d.pieceBudget = d.timeScale < 1.0f ? 30000u : 2500u;
    if (const char* v = std::getenv("EXPLOSION_PIECES")) d.pieceBudget = uint32_t(std::max(100, std::atoi(v)));
    if (const char* v = std::getenv("EXPLOSION_SETTLE")) d.settle = std::atoi(v) != 0;
    // A piece of this many voxels or fewer is dust: removed, nothing made of it. Bodies too small to see from
    // where a camera on this scale stands, and the first the piece budget would clear anyway.
    physics.dustVoxels = 8;
    physics.fracture.minImpactBreak = 2.0f;
    // A flattened building is one strike reaching all of it (strikeWhole): up to ninety metres of voxels across,
    // in up to a few thousand pieces.
    physics.fracture.maxShards = 4096;
    physics.fracture.maxReachVoxels = 192.0f;
    // Thirty steps a second, not sixty: with thousands of bodies in a falling city the physics is most of the
    // frame, and the drawn poses are interpolated between steps anyway. And at most three steps a frame: in the
    // heaviest moments the simulation slows (a moment of slow motion at the blast) rather than the frame rate
    // collapsing under steps it can never catch up on.
    if (d.timeScale < 1.0f) d.stepSeconds = 1.0f / 60.0f;   // slow motion: the finer step
    app.time().fixedDelta = d.stepSeconds;
    app.time().maxFixedStepsPerFrame = d.timeScale < 1.0f ? 8 : 3;
    app.time().scale = d.timeScale;
    if (d.recordFps > 0.0f) {
        app.time().maxDelta = 0.002f;
        app.time().scale = 1.0f / (d.recordFps * app.time().maxDelta);
    }
    runtime::installPhysics(app, physics);
    runtime::installImpactFracture(app);
    // Before the materials, which name them: city concrete breaks by "demolish"; the earth and a house's
    // floors by "rubble".
    runtime::registerFracturePattern(world, "demolish", demolish);
    runtime::registerFracturePattern(world, "rubble", rubble);
    if (!runtime::loadPhysicsMaterials(world, (here / "physics_materials.json").string()))
        projv::core::error("explosion: physics_materials.json did not load; nothing will break");

    app.events().on<projv::BodyBroken>([&world](const projv::BodyBroken& e) { onBrokenBody(world, e); });
    app.events().on<projv::BodyEmptied>([&world](const projv::BodyEmptied& e) {
        if (world.valid(e.entity)) world.destroy(e.entity);
    });
    app.events().on<projv::SimulationSlow>([](const projv::SimulationSlow& e) {
        static bool said = false;
        if (!said) projv::core::warn("explosion: the simulation is running slower than real time ({} steps dropped)", e.droppedSteps);
        said = true;
    });

    night_renderer::install(world, renderInstance, here);
    world.ctx().emplace<projv::GPUData>(projv::graphics::createTexturesForScene(scene));
    buildWorld(world, *startingMap);
    if (!d.unattended) printHelp();
}

// EXPLOSION_GPU=1: each pass's GPU time, averaged over a second, printed every second.
void gpuTimes(projv::Application& app) {
    static const bool wanted = std::getenv("EXPLOSION_GPU") != nullptr;
    if (!wanted) return;
    static double passMs[16] = {}, frameMs = 0.0, clock = 0.0;
    static int samples = 0;
    if (app.frameCount == 1) bgfx::setDebug(BGFX_DEBUG_PROFILER);
    const bgfx::Stats* stats = bgfx::getStats();
    if (stats && stats->gpuTimerFreq > 0) {
        const double toMs = 1000.0 / double(stats->gpuTimerFreq);
        frameMs += double(stats->gpuTimeEnd - stats->gpuTimeBegin) * toMs;
        for (uint16_t v = 0; v < stats->numViews; v++)
            if (stats->viewStats[v].view < 16)
                passMs[stats->viewStats[v].view] += double(stats->viewStats[v].gpuTimeEnd - stats->viewStats[v].gpuTimeBegin) * toMs;
        samples++;
    }
    if ((clock += app.time().unscaledDelta) < 1.0 || samples == 0) return;
    static const char* names[] = {"gbuffer", "ssao", "raygi", "giaccum", "giblur1", "giblur2", "shade", "smoke", "reconstruct", "taa", "display"};
    std::string line = fmt::format("gpu: t={:.1f} frame {:.2f} ms |", app.world.ctx().get<Demo>().clock, frameMs / samples);
    for (int v = 0; v < 11; v++) line += fmt::format(" {} {:.2f}", names[v], passMs[v] / samples);
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
    clock = 0.0;
    samples = 0;
    frameMs = 0.0;
    std::fill(std::begin(passMs), std::end(passMs), 0.0);
}

void render(projv::Application& app) {
    World& world = app.world;
    auto& renderInstance = world.ctx().get<projv::graphics::RenderInstance>();
    auto& scene = world.ctx().get<projv::Scene>();
    auto& gpuData = world.ctx().get<projv::GPUData>();
    const auto& camera = world.ctx().get<Camera>();
    auto& d = world.ctx().get<Demo>();
    d.frameMs = 1000.0 * app.time().unscaledDelta;
    const auto flushStart = Clock::now();
    projv::graphics::flushSceneUpdates(scene, gpuData);
    d.flushMs = msSince(flushStart);
    vec3 position = camera.position, direction = camera.forward();
    // The blast shakes the drawn camera (in the simulation's time, as the eye adapts).
    Shake& shake = world.ctx().get<Shake>();
    shake.clock += app.time().delta;
    shake.trauma = std::max(0.0f, shake.trauma - 0.9f * app.time().delta);
    if (shake.trauma > 0.0f) {
        const float amount = shake.trauma * shake.trauma, t = shake.clock;
        auto noise = [t](float seed) {
            return 0.5f * std::sin(t * 37.0f + seed) + 0.3f * std::sin(t * 61.0f + 2.1f * seed) + 0.2f * std::sin(t * 97.0f + 3.7f * seed);
        };
        position += amount * 0.35f * d.S * vec3(noise(1.0f), noise(2.0f), noise(3.0f));
        Camera shaken = camera;
        shaken.yaw += amount * 0.02f * noise(4.0f);
        shaken.pitch += amount * 0.02f * noise(5.0f);
        direction = shaken.forward();
    }
    const auto renderStart = Clock::now();
    night_renderer::upload(world, renderInstance, position, direction);
    projv::graphics::renderConstructedRenderer(renderInstance, renderInstance.getActiveRenderer(), &gpuData);
    d.renderMs = msSince(renderStart);
    gpuTimes(app);
    if (renderInstance.shouldClose) app.closeRequested = true;
}

void shutdown(projv::Application& app) {
    projv::graphics::destroyGPUData(app.world.ctx().get<projv::GPUData>());
    // Stopped here rather than left to process exit (Voxel Mayhem found the NVIDIA driver crashing with bgfx's
    // render thread still presenting while memory was freed under it).
    bgfx::shutdown();
    glfwTerminate();
}

// --name value, or --name=value: the value, or null when argv[i] is not --name.
const char* option(int argc, char** argv, int& i, const char* name) {
    const std::string arg = argv[i], flag = std::string("--") + name;
    if (arg == flag && i + 1 < argc) return argv[++i];
    if (arg.rfind(flag + "=", 0) == 0) return argv[i] + flag.size() + 1;
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (const char* value = option(argc, argv, i, "graphics")) {
            graphicsLevel = std::atoi(value);
            if (graphicsLevel < 1 || graphicsLevel > 3) {
                std::fprintf(stderr, "explosion: --graphics takes 1, 2 or 3\n");
                return 1;
            }
        } else if (const char* value = option(argc, argv, i, "map")) {
            startingMap = mapNamed(value);
            if (!startingMap) {
                std::fprintf(stderr, "explosion: --map takes city, house or forest\n");
                return 1;
            }
        } else {
            std::fprintf(stderr, "explosion: unknown argument '%s' (try --map city|house|forest, --graphics 1|2|3)\n", argv[i]);
            return 1;
        }
    }
    projv::Application app;
    app.addSystem(projv::Stage::Startup,     "startup",       startup);
    // "physics" is added at startup, between whatever names it in `before` and in `after`: those before it
    // must be added ahead of those after it.
    app.addSystem(projv::Stage::FixedUpdate, "explosion",     fixedTick, {.before = "physics"});
    app.addSystem(projv::Stage::FixedUpdate, "step start",    stepStart, {.before = "physics"});
    app.addSystem(projv::Stage::FixedUpdate, "step end",      stepEnd, {.after = "physics"});
    app.addSystem(projv::Stage::Update,      "controls",      controls);
    app.addSystem(projv::Stage::Update,      "title bar",     titleBar);
    app.addSystem(projv::Stage::Update,      "unattended",    unattendedTick);
    app.addSystem(projv::Stage::PostUpdate,  "cloud",         presentCloud, {.before = "scene bridge", .after = "physics: present"});
    app.addSystem(projv::Stage::PostUpdate,  "cloud grid",    buildCloudGrid, {.after = "cloud"});
    app.addSystem(projv::Stage::Render,      "render",        render);
    app.addSystem(projv::Stage::Shutdown,    "shutdown",      shutdown);
    app.run();
    return 0;
}
