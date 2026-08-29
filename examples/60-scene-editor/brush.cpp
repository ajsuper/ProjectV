#include "brush.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include "core/log.h"

namespace projv::editor {

const char* const BRUSH_SPACING_PARAM = "spacing";
const char* const BRUSH_DENSITY_PARAM = "density";

// =============================================================================
// Names
// =============================================================================

const char* brushKindName(BrushKind kind) {
    switch (kind) {
        case BrushKind::Material: return "material";
        case BrushKind::Geometry: return "geometry";
        case BrushKind::Scatter:  return "scatter";
    }
    return "material";
}

const char* brushKindLabel(BrushKind kind) {
    switch (kind) {
        case BrushKind::Material: return "Material";
        case BrushKind::Geometry: return "Geometry";
        case BrushKind::Scatter:  return "Scatter";
    }
    return "Material";
}

const char* brushKindHint(BrushKind kind) {
    switch (kind) {
        case BrushKind::Material:
            return "Recolours voxels that already exist. Cannot add or remove geometry, which makes\n"
                   "it the kind to write first: the worst a mistake can do is the wrong colour.";
        case BrushKind::Geometry:
            return "Decides whether each cell of the brush is solid. Sees empty cells too, so it can\n"
                   "build as well as carve.";
        case BrushKind::Scatter:
            return "Plants whole objects at points on the surface, each one checked for room before\n"
                   "it lands. Asked about sites rather than cells, and answers with a list of voxels.";
    }
    return "";
}

bool brushKindFromName(const std::string& name, BrushKind& out) {
    for (int i = 0; i < BRUSH_KIND_COUNT; i++) {
        BrushKind kind = static_cast<BrushKind>(i);
        if (name == brushKindName(kind)) { out = kind; return true; }
    }
    return false;
}

bool brushKindIsRunnable(BrushKind kind) {
    // All three now. Kept as a function rather than folded away because the Lab and the evaluator both
    // ask, and a fourth kind would land here first.
    (void)kind;
    return true;
}

const char* brushContextFieldName(BrushContextField field) {
    switch (field) {
        case BrushContextField::Position:  return "position";
        case BrushContextField::World:     return "world";
        case BrushContextField::Material:  return "material";
        case BrushContextField::Solid:     return "solid";
        case BrushContextField::SkinDepth: return "skinDepth";
        case BrushContextField::Crevice:   return "crevice";
        case BrushContextField::Distance:  return "distance";
        case BrushContextField::Normal:    return "normal";
    }
    return "";
}

const char* brushContextFieldHint(BrushContextField field) {
    switch (field) {
        case BrushContextField::Position:
            return "ctx.x, ctx.y, ctx.z -- the component's own voxel lattice. The same voxel is the\n"
                   "same coordinate for the life of the .data, so a texture built on it is coherent\n"
                   "across the whole model and across separate strokes.";
        case BrushContextField::World:
            return "ctx.wx, ctx.wy, ctx.wz -- world units. Use it for things that belong to the\n"
                   "world rather than the object (which way is up, a global gradient). Note that a\n"
                   "texture built on world position re-textures itself when the object is moved.";
        case BrushContextField::Material:
            return "ctx.slot, ctx.r, ctx.g, ctx.b, ctx.material -- what is in the cell now. This is\n"
                   "how a brush applies to stone and leaves the timber alone.";
        case BrushContextField::Solid:
            return "ctx.solid -- whether the cell is occupied. Free, and always present for a\n"
                   "geometry brush.";
        case BrushContextField::SkinDepth:
            return "ctx.depth -- 0 for a voxel with an exposed face, 1 for one directly behind it,\n"
                   "and so on. Costs a margin around the brush: see Max skin depth.";
        case BrushContextField::Crevice:
            return "ctx.crevice -- the fraction of nearby cells that are solid, 0 on an exposed\n"
                   "corner and 1 deep inside. This is the number that makes creases darker.";
        case BrushContextField::Distance:
            return "ctx.distance -- 0 at the centre of the dab, 1 at its rim. Free, and what a\n"
                   "falloff is built from.";
        case BrushContextField::Normal:
            return "ctx.nx, ctx.ny, ctx.nz -- which way the surface faces, from the local gradient.\n"
                   "Roughly unit length on a surface voxel and zero deep inside.";
    }
    return "";
}

bool brushContextFieldFromName(const std::string& name, BrushContextField& out) {
    for (int i = 0; i < BRUSH_CONTEXT_FIELD_COUNT; i++) {
        BrushContextField field = static_cast<BrushContextField>(i);
        if (name == brushContextFieldName(field)) { out = field; return true; }
    }
    return false;
}

const char* brushParamTypeName(BrushParamType type) {
    switch (type) {
        case BrushParamType::Float: return "float";
        case BrushParamType::Int:   return "int";
        case BrushParamType::Bool:  return "bool";
        case BrushParamType::Color: return "color";
        case BrushParamType::Enum:  return "enum";
        case BrushParamType::Text:  return "text";
        case BrushParamType::Asset: return "asset";
        case BrushParamType::Seed:  return "seed";
    }
    return "float";
}

const char* brushParamTypeLabel(BrushParamType type) {
    switch (type) {
        case BrushParamType::Float: return "Number";
        case BrushParamType::Int:   return "Whole number";
        case BrushParamType::Bool:  return "Checkbox";
        case BrushParamType::Color: return "Colour";
        case BrushParamType::Enum:  return "Choice";
        case BrushParamType::Text:  return "Text";
        case BrushParamType::Asset: return "Asset path";
        case BrushParamType::Seed:  return "Seed";
    }
    return "Number";
}

bool brushParamTypeFromName(const std::string& name, BrushParamType& out) {
    for (int i = 0; i < BRUSH_PARAM_TYPE_COUNT; i++) {
        BrushParamType type = static_cast<BrushParamType>(i);
        if (name == brushParamTypeName(type)) { out = type; return true; }
    }
    return false;
}

// =============================================================================
// The Lua state
// =============================================================================

// Everything one loaded brush holds on the Lua side. Kept behind a shared_ptr so an invocation can
// hold the state alive for the length of a stroke even if the file is reloaded underneath it -- a
// hot reload during a drag would otherwise close the state the stroke is mid-way through calling.
struct BrushScript {
    lua_State* state = nullptr;
    int applyRef = LUA_NOREF;
    int contextRef = LUA_NOREF;    // The one context table, reused per voxel.
    int paramsRef = LUA_NOREF;     // The parameter table, rebuilt per stroke.
    BrushDefinition* owner = nullptr;

    // The scene query a scatter script reaches through pv.solid. Empty outside a stroke.
    BrushSolidQuery solidQuery;

    // The deadline for the call in progress. See BRUSH_CALL_TIMEOUT_SECONDS.
    std::chrono::steady_clock::time_point callStart;

    ~BrushScript() {
        if (state) lua_close(state);
    }
};

namespace {

// The state's own back-pointer, in Lua's per-state extra space. This is how the count hook and the
// redirected print find the brush they belong to without a global table.
BrushScript* scriptOf(lua_State* state) {
    return *static_cast<BrushScript**>(lua_getextraspace(state));
}

void appendMessage(BrushDefinition* owner, bool isError, std::string text) {
    if (!owner) return;
    // Bounded: a brush that prints per voxel would otherwise grow this without limit, and the panel
    // only ever shows the tail of it anyway.
    const size_t MESSAGE_LIMIT = 400;
    if (owner->messages.size() >= MESSAGE_LIMIT) {
        owner->messages.erase(owner->messages.begin(),
                              owner->messages.begin() + (owner->messages.size() - MESSAGE_LIMIT + 1));
    }
    BrushMessage message;
    message.isError = isError;
    message.text = std::move(text);
    owner->messages.push_back(std::move(message));
}

// --- The watchdog ---------------------------------------------------------------------------

void instructionHook(lua_State* state, lua_Debug* /*debug*/) {
    BrushScript* script = scriptOf(state);
    if (!script) return;
    std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - script->callStart;
    if (elapsed.count() > BRUSH_CALL_TIMEOUT_SECONDS) {
        // %d, not %.0f: luaL_error formats through lua_pushfstring, which handles a short list of
        // specifiers and no precision modifiers at all -- a "%.0f" here does not print a number, it
        // replaces the whole message with "invalid option '%.' to 'lua_pushfstring'", so the one
        // error the user is most likely to hit would arrive unreadable.
        // Names the other cause as well as the likely one. At a quarter of a second an overrun was
        // an endless loop and nothing else; at two, a scatter brush building a genuinely large object
        // can reach it honestly, and a reader told only about loops will go looking for a bug that is
        // not there instead of turning the size down.
        luaL_error(state, "brush call exceeded %d ms -- an endless loop, or one placement asking for "
                          "more than can be built in that time",
                   int(BRUSH_CALL_TIMEOUT_SECONDS * 1000.0));
    }
}

// --- The sandbox ----------------------------------------------------------------------------

// print, redirected into the brush's own output log. Scripts print while they are being written --
// that is what the Output tab is for -- and a print that went to a terminal nobody is looking at
// would make the Lab's whole loop worse.
int luaPrint(lua_State* state) {
    BrushScript* script = scriptOf(state);
    int count = lua_gettop(state);
    std::string line;
    for (int i = 1; i <= count; i++) {
        size_t length = 0;
        const char* text = luaL_tolstring(state, i, &length);   // Honours __tostring.
        if (i > 1) line += '\t';
        line.append(text, length);
        lua_pop(state, 1);
    }
    appendMessage(script ? script->owner : nullptr, false, std::move(line));
    return 0;
}

// --- Noise ----------------------------------------------------------------------------------
//
// Native, and exposed to the script rather than left for it to implement. This is the whole
// performance answer for a scripted brush: the expensive part of a procedural texture is the noise,
// a script that wrote its own would spend thousands of VM instructions per voxel on it, and the same
// function in C++ is tens of nanoseconds. What is left for the script is composition -- which noise,
// at what frequency, thresholded where -- which is the part that is actually the brush's design.

// A 3D integer hash. Three rounds of multiply-xor-shift; the constants are the usual large odd
// primes. Deterministic across runs and platforms, which a brush depends on: the same stroke on the
// same model has to produce the same texture tomorrow.
uint32_t hashCoords(int32_t x, int32_t y, int32_t z, uint32_t seed) {
    uint32_t h = seed * 0x9E3779B9u;
    h ^= uint32_t(x) * 0x85EBCA6Bu; h = (h << 13) | (h >> 19); h *= 0xC2B2AE35u;
    h ^= uint32_t(y) * 0x27D4EB2Fu; h = (h << 17) | (h >> 15); h *= 0x165667B1u;
    h ^= uint32_t(z) * 0x9E3779B1u; h = (h << 11) | (h >> 21); h *= 0x85EBCA77u;
    h ^= h >> 16;
    return h;
}

float hashUnit(int32_t x, int32_t y, int32_t z, uint32_t seed) {
    return float(hashCoords(x, y, z, seed) & 0xFFFFFFu) / float(0xFFFFFF);
}

int32_t floorToInt(double value) {
    return int32_t(std::floor(value));
}

float smoothFade(float t) {
    return t * t * (3.0f - 2.0f * t);
}

// Value noise: lattice hashes, smoothly interpolated. Not gradient noise -- it has a slightly blockier
// character -- but it is a third of the arithmetic and for a voxel texture whose output is quantised
// into a palette ramp anyway the difference does not survive.
float valueNoise(double px, double py, double pz, uint32_t seed) {
    int32_t x0 = floorToInt(px), y0 = floorToInt(py), z0 = floorToInt(pz);
    float fx = smoothFade(float(px - x0));
    float fy = smoothFade(float(py - y0));
    float fz = smoothFade(float(pz - z0));

    float c[8];
    for (int i = 0; i < 8; i++) {
        c[i] = hashUnit(x0 + (i & 1), y0 + ((i >> 1) & 1), z0 + ((i >> 2) & 1), seed);
    }
    float x00 = c[0] + (c[1] - c[0]) * fx;
    float x10 = c[2] + (c[3] - c[2]) * fx;
    float x01 = c[4] + (c[5] - c[4]) * fx;
    float x11 = c[6] + (c[7] - c[6]) * fx;
    float y0v = x00 + (x10 - x00) * fy;
    float y1v = x01 + (x11 - x01) * fy;
    return y0v + (y1v - y0v) * fz;
}

// Worley (cellular) noise: distance to the nearest and second-nearest feature point, one point per
// lattice cell, in cell units. f2 - f1 is the classic crack: it is near zero exactly on the boundary
// between two cells, which is a connected network of thin lines through the volume -- which is what
// a crack in rock is.
void worleyNoise(double px, double py, double pz, uint32_t seed, float& f1, float& f2) {
    int32_t cx = floorToInt(px), cy = floorToInt(py), cz = floorToInt(pz);
    f1 = 1.0e9f;
    f2 = 1.0e9f;
    for (int dz = -1; dz <= 1; dz++) {
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int32_t gx = cx + dx, gy = cy + dy, gz = cz + dz;
                uint32_t h = hashCoords(gx, gy, gz, seed);
                // Three independent offsets out of one hash: bytes 0, 1 and 2. A second and third
                // hash call would cost twice as much for feature points nobody can tell apart.
                float ox = float(h & 0xFFu) / 255.0f;
                float oy = float((h >> 8) & 0xFFu) / 255.0f;
                float oz = float((h >> 16) & 0xFFu) / 255.0f;
                float vx = float(gx) + ox - float(px);
                float vy = float(gy) + oy - float(py);
                float vz = float(gz) + oz - float(pz);
                float distanceSquared = vx * vx + vy * vy + vz * vz;
                if (distanceSquared < f1) { f2 = f1; f1 = distanceSquared; }
                else if (distanceSquared < f2) { f2 = distanceSquared; }
            }
        }
    }
    f1 = std::sqrt(f1);
    f2 = std::sqrt(f2);
}

int luaHash(lua_State* state) {
    double x = luaL_checknumber(state, 1);
    double y = luaL_checknumber(state, 2);
    double z = luaL_checknumber(state, 3);
    uint32_t seed = uint32_t(luaL_optinteger(state, 4, 0));
    lua_pushnumber(state, hashUnit(floorToInt(x), floorToInt(y), floorToInt(z), seed));
    return 1;
}

int luaValueNoise(lua_State* state) {
    double x = luaL_checknumber(state, 1);
    double y = luaL_checknumber(state, 2);
    double z = luaL_checknumber(state, 3);
    uint32_t seed = uint32_t(luaL_optinteger(state, 4, 0));
    lua_pushnumber(state, valueNoise(x, y, z, seed));
    return 1;
}

int luaFbm(lua_State* state) {
    double x = luaL_checknumber(state, 1);
    double y = luaL_checknumber(state, 2);
    double z = luaL_checknumber(state, 3);
    int octaves = int(luaL_optinteger(state, 4, 4));
    uint32_t seed = uint32_t(luaL_optinteger(state, 5, 0));
    octaves = std::clamp(octaves, 1, 8);

    float sum = 0.0f;
    float amplitude = 0.5f;
    float total = 0.0f;
    double frequency = 1.0;
    for (int i = 0; i < octaves; i++) {
        sum += amplitude * valueNoise(x * frequency, y * frequency, z * frequency, seed + uint32_t(i) * 7919u);
        total += amplitude;
        amplitude *= 0.5f;
        frequency *= 2.0;
    }
    lua_pushnumber(state, total > 0.0f ? sum / total : 0.0f);
    return 1;
}

int luaWorley(lua_State* state) {
    double x = luaL_checknumber(state, 1);
    double y = luaL_checknumber(state, 2);
    double z = luaL_checknumber(state, 3);
    uint32_t seed = uint32_t(luaL_optinteger(state, 4, 0));
    float f1 = 0.0f, f2 = 0.0f;
    worleyNoise(x, y, z, seed, f1, f2);
    lua_pushnumber(state, f1);
    lua_pushnumber(state, f2);
    return 2;
}

int luaClamp(lua_State* state) {
    double value = luaL_checknumber(state, 1);
    double low = luaL_optnumber(state, 2, 0.0);
    double high = luaL_optnumber(state, 3, 1.0);
    lua_pushnumber(state, value < low ? low : (value > high ? high : value));
    return 1;
}

int luaLerp(lua_State* state) {
    double a = luaL_checknumber(state, 1);
    double b = luaL_checknumber(state, 2);
    double t = luaL_checknumber(state, 3);
    lua_pushnumber(state, a + (b - a) * t);
    return 1;
}

// GLSL's smoothstep, argument order included, because that is the one every shader author already
// knows: edge0, edge1, x.
int luaSmoothstep(lua_State* state) {
    double edge0 = luaL_checknumber(state, 1);
    double edge1 = luaL_checknumber(state, 2);
    double x = luaL_checknumber(state, 3);
    double t = edge1 == edge0 ? (x < edge0 ? 0.0 : 1.0) : (x - edge0) / (edge1 - edge0);
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    lua_pushnumber(state, t * t * (3.0 - 2.0 * t));
    return 1;
}

// pv.solid(x, y, z) -- is that cell of the component occupied?
//
// The fit test, and the only call in the library that is not a pure function of its arguments: it asks
// the scene. That is a real exception to the purity rule and it is narrow on purpose -- the answer is
// stable for the whole of a dab (nothing is written until every site has been decided), so a brush
// that uses it is still reproducible against the geometry it was run on.
//
// False when no query is installed, which is the safe direction: a fit test that cannot see the scene
// reporting everything occupied would silently refuse every placement, and a brush that plants nothing
// looks exactly like a brush whose parameters are wrong.
int luaSolid(lua_State* state) {
    BrushScript* script = scriptOf(state);
    lua_Integer x = luaL_checkinteger(state, 1);
    lua_Integer y = luaL_checkinteger(state, 2);
    lua_Integer z = luaL_checkinteger(state, 3);
    bool solid = false;
    if (script && script->solidQuery) {
        solid = script->solidQuery(int32_t(x), int32_t(y), int32_t(z));
    }
    lua_pushboolean(state, solid ? 1 : 0);
    return 1;
}

// pv.fits(x0,y0,z0, x1,y1,z1) -- is every cell of that box empty?
//
// Native rather than a Lua loop over pv.solid, because it is the shape of the question a scatter brush
// actually asks ("is there room for this") and because a tree's box is a few thousand cells -- which is
// a few thousand VM iterations per candidate site in a loop, against a tight C one here.
int luaFits(lua_State* state) {
    BrushScript* script = scriptOf(state);
    int32_t x0 = int32_t(luaL_checkinteger(state, 1));
    int32_t y0 = int32_t(luaL_checkinteger(state, 2));
    int32_t z0 = int32_t(luaL_checkinteger(state, 3));
    int32_t x1 = int32_t(luaL_checkinteger(state, 4));
    int32_t y1 = int32_t(luaL_checkinteger(state, 5));
    int32_t z1 = int32_t(luaL_checkinteger(state, 6));
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    if (z1 < z0) std::swap(z0, z1);

    // Bounded so a typo cannot ask about a billion cells inside the per-call deadline. Generous
    // enough that an honest question about a large placement -- a tree's headroom on a scene-sized
    // component -- is answered rather than refused; the deadline is the real guard on a slow one.
    const int64_t MAX_CELLS = 16000000;
    int64_t cells = int64_t(x1 - x0 + 1) * int64_t(y1 - y0 + 1) * int64_t(z1 - z0 + 1);
    if (cells > MAX_CELLS) {
        return luaL_error(state, "pv.fits was asked about %d cells; the ceiling is %d",
                          int(cells), int(MAX_CELLS));
    }

    bool fits = true;
    if (script && script->solidQuery) {
        for (int32_t z = z0; z <= z1 && fits; z++) {
            for (int32_t y = y0; y <= y1 && fits; y++) {
                for (int32_t x = x0; x <= x1; x++) {
                    if (script->solidQuery(x, y, z)) { fits = false; break; }
                }
            }
        }
    }
    lua_pushboolean(state, fits ? 1 : 0);
    return 1;
}

const luaL_Reg PV_LIBRARY[] = {
    { "solid",      luaSolid },
    { "fits",       luaFits },
    { "hash",       luaHash },
    { "noise",      luaValueNoise },
    { "fbm",        luaFbm },
    { "worley",     luaWorley },
    { "clamp",      luaClamp },
    { "lerp",       luaLerp },
    { "smoothstep", luaSmoothstep },
    { nullptr,      nullptr }
};

// Every global a brush is not allowed to see. Removed after luaL_openlibs rather than by building an
// environment from nothing, because the list of things a brush *should* have (math, string, table,
// pairs, pcall, ...) is long and dull and gets stale, while the list of things it must not have is
// short and is the part worth being explicit about.
//
// This is a guard against accidents and sharp edges, not a security boundary. A brush is a file the
// user put in their own brushes folder, and the sandbox's job is that a brush cannot delete their
// work by mistake, cannot depend on the wall clock and quietly stop being reproducible, and cannot
// reach outside the folder it lives in. Someone determined to do harm with a Lua file they installed
// themselves is not the threat this answers -- and `package.loadlib`, the one route that would make
// the point moot, is not compiled in at all (see LUA_CFLAGS in the Makefile).
const char* const SANDBOX_REMOVALS[] = {
    "dofile", "loadfile", "load", "require", "package", "io", "debug", "collectgarbage",
    // os is removed whole rather than trimmed to os.time/os.clock: a brush must be a pure function of
    // its context, and the clock is exactly the thing that would let one stop being one.
    "os",
    nullptr
};

lua_State* newSandboxedState(BrushScript* script) {
    lua_State* state = luaL_newstate();
    if (!state) return nullptr;
    *static_cast<BrushScript**>(lua_getextraspace(state)) = script;

    luaL_openlibs(state);
    for (const char* const* name = SANDBOX_REMOVALS; *name; name++) {
        lua_pushnil(state);
        lua_setglobal(state, *name);
    }
    lua_pushcfunction(state, luaPrint);
    lua_setglobal(state, "print");

    luaL_newlib(state, PV_LIBRARY);
    lua_setglobal(state, "pv");

    lua_sethook(state, instructionHook, LUA_MASKCOUNT, BRUSH_HOOK_INTERVAL);
    return state;
}

// --- Reading the declaration ----------------------------------------------------------------
//
// Every one of these leaves the stack exactly as it found it. The table being read is at `index`.

bool fieldString(lua_State* state, int index, const char* key, std::string& out) {
    lua_getfield(state, index, key);
    bool found = lua_type(state, -1) == LUA_TSTRING;
    if (found) out = lua_tostring(state, -1);
    lua_pop(state, 1);
    return found;
}

bool fieldNumber(lua_State* state, int index, const char* key, double& out) {
    lua_getfield(state, index, key);
    bool found = lua_isnumber(state, -1) != 0;
    if (found) out = lua_tonumber(state, -1);
    lua_pop(state, 1);
    return found;
}

bool fieldBool(lua_State* state, int index, const char* key, bool& out) {
    lua_getfield(state, index, key);
    bool found = lua_isboolean(state, -1);
    if (found) out = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    return found;
}

// A colour, as {r, g, b} in 0..1. Also accepts a single number for a grey, because a brush that wants
// black should be able to say 0.
bool fieldColor(lua_State* state, int index, const char* key, float out[3]) {
    lua_getfield(state, index, key);
    bool found = false;
    if (lua_isnumber(state, -1)) {
        float grey = float(lua_tonumber(state, -1));
        out[0] = out[1] = out[2] = grey;
        found = true;
    } else if (lua_istable(state, -1)) {
        for (int i = 0; i < 3; i++) {
            lua_rawgeti(state, -1, i + 1);
            if (lua_isnumber(state, -1)) out[i] = float(lua_tonumber(state, -1));
            lua_pop(state, 1);
        }
        found = true;
    }
    lua_pop(state, 1);
    if (found) {
        for (int i = 0; i < 3; i++) out[i] = std::clamp(out[i], 0.0f, 1.0f);
    }
    return found;
}

// One entry of the `params` list. The table is on top of the stack.
bool readParam(lua_State* state, BrushParam& param, std::string& error) {
    if (!lua_istable(state, -1)) {
        error = "each entry of `params` must be a table";
        return false;
    }
    if (!fieldString(state, -1, "name", param.name) || param.name.empty()) {
        error = "a parameter is missing its `name`";
        return false;
    }
    std::string typeName = "float";
    fieldString(state, -1, "type", typeName);
    if (!brushParamTypeFromName(typeName, param.type)) {
        error = "parameter '" + param.name + "' has unknown type '" + typeName + "'";
        return false;
    }
    if (!fieldString(state, -1, "label", param.label)) param.label = param.name;
    fieldString(state, -1, "tooltip", param.tooltip);

    switch (param.type) {
        case BrushParamType::Float:
        case BrushParamType::Int:
        case BrushParamType::Seed: {
            // Ranges have to be given for a Float (there is no sensible universal one), but Int and
            // Seed both have obvious ones and a brush should not be made to restate them.
            double low = param.type == BrushParamType::Float ? 0.0 : 0.0;
            double high = param.type == BrushParamType::Float ? 1.0
                        : (param.type == BrushParamType::Seed ? 1000000.0 : 64.0);
            fieldNumber(state, -1, "min", low);
            fieldNumber(state, -1, "max", high);
            param.minimum = low;
            param.maximum = std::max(low, high);
            param.step = param.type == BrushParamType::Float
                       ? std::max(1.0e-6, (param.maximum - param.minimum) / 200.0) : 1.0;
            fieldNumber(state, -1, "step", param.step);
            param.defaultNumber = param.minimum;
            fieldNumber(state, -1, "default", param.defaultNumber);
            param.defaultNumber = std::clamp(param.defaultNumber, param.minimum, param.maximum);
            break;
        }
        case BrushParamType::Bool:
            fieldBool(state, -1, "default", param.defaultBoolean);
            break;
        case BrushParamType::Color:
            fieldColor(state, -1, "default", param.defaultColor);
            break;
        case BrushParamType::Enum: {
            lua_getfield(state, -1, "options");
            if (lua_istable(state, -1)) {
                lua_Integer count = luaL_len(state, -1);
                for (lua_Integer i = 1; i <= count; i++) {
                    lua_rawgeti(state, -1, i);
                    if (lua_type(state, -1) == LUA_TSTRING) param.options.push_back(lua_tostring(state, -1));
                    lua_pop(state, 1);
                }
            }
            lua_pop(state, 1);
            if (param.options.empty()) {
                error = "parameter '" + param.name + "' is a choice with no `options`";
                return false;
            }
            param.defaultText = param.options.front();
            fieldString(state, -1, "default", param.defaultText);
            if (std::find(param.options.begin(), param.options.end(), param.defaultText) ==
                param.options.end()) {
                param.defaultText = param.options.front();
            }
            break;
        }
        case BrushParamType::Text:
        case BrushParamType::Asset:
            fieldString(state, -1, "default", param.defaultText);
            break;
    }
    return true;
}

// One entry of the `materials` list. The table is on top of the stack.
bool readMaterial(lua_State* state, BrushMaterial& material, std::string& error) {
    if (!lua_istable(state, -1)) {
        error = "each entry of `materials` must be a table";
        return false;
    }
    if (!fieldString(state, -1, "name", material.name) || material.name.empty()) {
        error = "a material is missing its `name`";
        return false;
    }
    fieldColor(state, -1, "color", material.color);

    double steps = 1.0;
    fieldNumber(state, -1, "steps", steps);
    material.steps = std::clamp(int(steps), 1, 64);
    if (material.steps > 1) {
        // A ramp without a second colour is a ramp to black, which is almost never meant. Falling
        // back to the first colour makes such a declaration a flat set of identical entries, which is
        // harmless and visibly wrong -- better than silently darkening.
        if (!fieldColor(state, -1, "colorTo", material.colorTo)) {
            std::memcpy(material.colorTo, material.color, sizeof(material.colorTo));
        }
    }

    double value = 0.0;
    if (fieldNumber(state, -1, "glossiness", value))  material.glossiness = float(std::clamp(value, 0.0, 1.0));
    if (fieldNumber(state, -1, "metallic", value))    material.metallic = float(std::clamp(value, 0.0, 1.0));
    if (fieldNumber(state, -1, "transparency", value)) material.transparency = float(std::clamp(value, 0.0, 1.0));
    if (fieldNumber(state, -1, "ior", value))         material.ior = float(std::clamp(value, 1.0, 3.0));
    if (fieldNumber(state, -1, "emissiveStrength", value)) material.emissiveStrength = float(std::max(0.0, value));
    material.hasEmissionColor = fieldColor(state, -1, "emission", material.emission);
    return true;
}

std::string sidecarPathFor(const std::string& sourcePath) {
    std::filesystem::path path(sourcePath);
    path.replace_extension();
    return path.string() + ".params.json";
}

int64_t fileModifiedTime(const std::string& path, size_t& sizeOut) {
    std::error_code code;
    auto time = std::filesystem::last_write_time(path, code);
    if (code) { sizeOut = 0; return 0; }
    sizeOut = size_t(std::filesystem::file_size(path, code));
    if (code) sizeOut = 0;
    return time.time_since_epoch().count();
}

// The value a parameter takes when nothing has been stored for it.
BrushParamValue defaultValueOf(const BrushParam& param) {
    BrushParamValue value;
    switch (param.type) {
        case BrushParamType::Float:
        case BrushParamType::Int:
        case BrushParamType::Seed:
            value.number = param.defaultNumber;
            break;
        case BrushParamType::Bool:
            value.boolean = param.defaultBoolean;
            break;
        case BrushParamType::Color:
            std::memcpy(value.color, param.defaultColor, sizeof(value.color));
            break;
        case BrushParamType::Enum:
        case BrushParamType::Text:
        case BrushParamType::Asset:
            value.text = param.defaultText;
            break;
    }
    return value;
}

} // namespace

int BrushDefinition::totalMaterialSlots() const {
    int total = 0;
    for (const BrushMaterial& material : materials) total += std::max(1, material.steps);
    // Colour parameters are material roles too, and they count against the same palette ceiling. See
    // brushExpandMaterials.
    for (const BrushParam& param : params) {
        if (param.type == BrushParamType::Color) total++;
    }
    return total;
}

void brushExpandMaterials(const BrushDefinition& brush, std::vector<BrushMaterialSlot>& out) {
    out.clear();

    auto applyBinding = [&brush](BrushMaterialSlot& slot) {
        auto binding = brush.materialBindings.find(slot.declaredName);
        if (binding == brush.materialBindings.end() || binding->second.empty()) {
            slot.name = slot.declaredName;
            slot.bound = false;
            return;
        }
        slot.name = binding->second;
        slot.bound = true;
    };

    for (const BrushMaterial& material : brush.materials) {
        int steps = std::max(1, material.steps);
        for (int i = 0; i < steps; i++) {
            BrushMaterialSlot slot;
            slot.glossiness = material.glossiness;
            slot.metallic = material.metallic;
            slot.transparency = material.transparency;
            slot.ior = material.ior;
            slot.emissiveStrength = material.emissiveStrength;
            slot.hasEmissionColor = material.hasEmissionColor;
            std::memcpy(slot.emission, material.emission, sizeof(slot.emission));

            if (steps == 1) {
                slot.declaredName = material.name;
                std::memcpy(slot.color, material.color, sizeof(slot.color));
            } else {
                // Named with a one-based index so the palette reads in the order the ramp runs, and
                // so a brush's entries sort together in the palette panel.
                slot.declaredName = material.name + "." + std::to_string(i + 1);
                float t = float(i) / float(steps - 1);
                for (int channel = 0; channel < 3; channel++) {
                    slot.color[channel] = material.color[channel] +
                                          (material.colorTo[channel] - material.color[channel]) * t;
                }
            }
            applyBinding(slot);
            out.push_back(std::move(slot));
        }
    }

    // Colour parameters, after the declared roles and in declaration order. That ordering is the
    // contract `apply` addresses -- a script returning 1 gets the first declared material whether or
    // not the brush also has colour parameters, so adding one cannot renumber what is already written.
    for (size_t i = 0; i < brush.params.size(); i++) {
        if (brush.params[i].type != BrushParamType::Color) continue;
        BrushMaterialSlot slot;
        slot.declaredName = brush.params[i].name;
        slot.paramIndex = int(i);
        // The declared default is the creation colour, exactly as for a material role. The *current*
        // value is not: it is the colour the entry resolved to last time, which is a fact about the
        // palette and would be wrong to write back into a fresh entry under a different name.
        std::memcpy(slot.color, brush.params[i].defaultColor, sizeof(slot.color));
        applyBinding(slot);
        out.push_back(std::move(slot));
    }
}

void brushSetMaterialBinding(BrushDefinition& brush, const std::string& declaredName,
                             const std::string& paletteEntryName) {
    if (declaredName.empty()) return;
    if (paletteEntryName.empty()) brush.materialBindings.erase(declaredName);
    else brush.materialBindings[declaredName] = paletteEntryName;
    brush.valuesDirty = true;
}

std::string brushMaterialBinding(const BrushDefinition& brush, const std::string& declaredName) {
    auto binding = brush.materialBindings.find(declaredName);
    return binding == brush.materialBindings.end() ? std::string() : binding->second;
}

BrushDefinition* BrushLibrary::find(const std::string& id) {
    for (std::shared_ptr<BrushDefinition>& brush : brushes) {
        if (brush->id == id) return brush.get();
    }
    return nullptr;
}

const BrushDefinition* BrushLibrary::find(const std::string& id) const {
    for (const std::shared_ptr<BrushDefinition>& brush : brushes) {
        if (brush->id == id) return brush.get();
    }
    return nullptr;
}

// =============================================================================
// Sidecar
// =============================================================================
//
// Values and editor-defined parameters, in a .params.json beside the script. Two files rather than
// one because they have different authors: the .lua is written by whoever wrote the brush and belongs
// in version control beside it, while the values are the local user's dial settings and change every
// time a slider moves. Writing settings back into the script would mean an editor that rewrites the
// author's source on every drag.

namespace {

void writeParamValue(nlohmann::json& target, const BrushParam& param, const BrushParamValue& value) {
    switch (param.type) {
        case BrushParamType::Float:
        case BrushParamType::Int:
        case BrushParamType::Seed:
            target = value.number;
            break;
        case BrushParamType::Bool:
            target = value.boolean;
            break;
        case BrushParamType::Color:
            target = { value.color[0], value.color[1], value.color[2] };
            break;
        case BrushParamType::Enum:
        case BrushParamType::Text:
        case BrushParamType::Asset:
            target = value.text;
            break;
    }
}

bool readParamValue(const nlohmann::json& source, const BrushParam& param, BrushParamValue& value) {
    switch (param.type) {
        case BrushParamType::Float:
        case BrushParamType::Int:
        case BrushParamType::Seed:
            if (!source.is_number()) return false;
            value.number = std::clamp(source.get<double>(), param.minimum, param.maximum);
            return true;
        case BrushParamType::Bool:
            if (!source.is_boolean()) return false;
            value.boolean = source.get<bool>();
            return true;
        case BrushParamType::Color:
            if (!source.is_array() || source.size() < 3) return false;
            for (int i = 0; i < 3; i++) {
                if (!source[i].is_number()) return false;
                value.color[i] = std::clamp(source[i].get<float>(), 0.0f, 1.0f);
            }
            return true;
        case BrushParamType::Enum: {
            if (!source.is_string()) return false;
            std::string text = source.get<std::string>();
            if (std::find(param.options.begin(), param.options.end(), text) == param.options.end()) {
                return false;
            }
            value.text = std::move(text);
            return true;
        }
        case BrushParamType::Text:
        case BrushParamType::Asset:
            if (!source.is_string()) return false;
            value.text = source.get<std::string>();
            return true;
    }
    return false;
}

void writeParamSchema(nlohmann::json& target, const BrushParam& param) {
    target["name"] = param.name;
    target["type"] = brushParamTypeName(param.type);
    if (!param.label.empty() && param.label != param.name) target["label"] = param.label;
    if (!param.tooltip.empty()) target["tooltip"] = param.tooltip;
    switch (param.type) {
        case BrushParamType::Float:
        case BrushParamType::Int:
        case BrushParamType::Seed:
            target["min"] = param.minimum;
            target["max"] = param.maximum;
            target["step"] = param.step;
            target["default"] = param.defaultNumber;
            break;
        case BrushParamType::Bool:
            target["default"] = param.defaultBoolean;
            break;
        case BrushParamType::Color:
            target["default"] = { param.defaultColor[0], param.defaultColor[1], param.defaultColor[2] };
            break;
        case BrushParamType::Enum:
            target["options"] = param.options;
            target["default"] = param.defaultText;
            break;
        case BrushParamType::Text:
        case BrushParamType::Asset:
            target["default"] = param.defaultText;
            break;
    }
}

bool readParamSchema(const nlohmann::json& source, BrushParam& param) {
    if (!source.is_object() || !source.contains("name") || !source["name"].is_string()) return false;
    param.name = source["name"].get<std::string>();
    if (param.name.empty()) return false;
    std::string typeName = source.value("type", std::string("float"));
    if (!brushParamTypeFromName(typeName, param.type)) return false;
    param.label = source.value("label", param.name);
    param.tooltip = source.value("tooltip", std::string());
    param.editorDefined = true;

    switch (param.type) {
        case BrushParamType::Float:
        case BrushParamType::Int:
        case BrushParamType::Seed:
            param.minimum = source.value("min", 0.0);
            param.maximum = std::max(param.minimum, source.value("max", 1.0));
            param.step = source.value("step", std::max(1.0e-6, (param.maximum - param.minimum) / 200.0));
            param.defaultNumber = std::clamp(source.value("default", param.minimum),
                                             param.minimum, param.maximum);
            break;
        case BrushParamType::Bool:
            param.defaultBoolean = source.value("default", false);
            break;
        case BrushParamType::Color:
            if (source.contains("default") && source["default"].is_array() &&
                source["default"].size() >= 3) {
                for (int i = 0; i < 3; i++) {
                    param.defaultColor[i] = std::clamp(source["default"][i].get<float>(), 0.0f, 1.0f);
                }
            }
            break;
        case BrushParamType::Enum:
            if (source.contains("options") && source["options"].is_array()) {
                for (const nlohmann::json& option : source["options"]) {
                    if (option.is_string()) param.options.push_back(option.get<std::string>());
                }
            }
            if (param.options.empty()) return false;
            param.defaultText = source.value("default", param.options.front());
            if (std::find(param.options.begin(), param.options.end(), param.defaultText) ==
                param.options.end()) {
                param.defaultText = param.options.front();
            }
            break;
        case BrushParamType::Text:
        case BrushParamType::Asset:
            param.defaultText = source.value("default", std::string());
            break;
    }
    return true;
}

// Applies the sidecar to a brush whose script-declared parameters are already in place: appends the
// editor-defined ones, then sets every value it has a match for. Anything it cannot match is dropped
// silently -- a stored value for a parameter the script has since removed, or whose type it changed,
// is not something to report to the user, it is just yesterday's setting.
void loadSidecar(BrushDefinition& brush) {
    std::ifstream file(brush.sidecarPath);
    if (!file.is_open()) return;

    nlohmann::json document;
    try {
        file >> document;
    } catch (const std::exception& error) {
        appendMessage(&brush, true, std::string("params file could not be read: ") + error.what());
        return;
    }
    if (!document.is_object()) return;

    if (document.contains("editorParams") && document["editorParams"].is_array()) {
        for (const nlohmann::json& entry : document["editorParams"]) {
            BrushParam param;
            if (!readParamSchema(entry, param)) continue;
            bool taken = false;
            for (const BrushParam& existing : brush.params) {
                if (existing.name == param.name) { taken = true; break; }
            }
            // A script that has since declared a parameter of the same name wins: the file is the
            // brush's own statement of what it takes, and the editor-side copy was a stand-in for it.
            if (taken) continue;
            brush.params.push_back(std::move(param));
        }
    }

    brush.values.assign(brush.params.size(), BrushParamValue());
    for (size_t i = 0; i < brush.params.size(); i++) {
        brush.values[i] = defaultValueOf(brush.params[i]);
    }
    // Bindings are kept whatever the script now says: a role the script has renamed simply has no
    // binding any more, and one it has removed leaves an entry nobody looks up. Dropping unmatched
    // ones would lose a binding across a typo in the .lua that is fixed a second later.
    if (document.contains("materialBindings") && document["materialBindings"].is_object()) {
        for (auto entry = document["materialBindings"].begin();
             entry != document["materialBindings"].end(); ++entry) {
            if (!entry.value().is_string()) continue;
            std::string target = entry.value().get<std::string>();
            if (!target.empty()) brush.materialBindings[entry.key()] = target;
        }
    }

    if (!document.contains("values") || !document["values"].is_object()) return;
    const nlohmann::json& values = document["values"];
    for (size_t i = 0; i < brush.params.size(); i++) {
        auto entry = values.find(brush.params[i].name);
        if (entry == values.end()) continue;
        BrushParamValue value = brush.values[i];
        if (readParamValue(*entry, brush.params[i], value)) brush.values[i] = value;
    }
}

} // namespace

bool brushSaveSidecar(const BrushDefinition& brush) {
    nlohmann::json document;
    nlohmann::json values = nlohmann::json::object();
    nlohmann::json editorParams = nlohmann::json::array();

    for (size_t i = 0; i < brush.params.size(); i++) {
        if (i < brush.values.size()) {
            writeParamValue(values[brush.params[i].name], brush.params[i], brush.values[i]);
        }
        if (brush.params[i].editorDefined) {
            nlohmann::json schema = nlohmann::json::object();
            writeParamSchema(schema, brush.params[i]);
            editorParams.push_back(std::move(schema));
        }
    }
    document["values"] = std::move(values);
    if (!editorParams.empty()) document["editorParams"] = std::move(editorParams);

    // Which palette entry each material role is bound to. Beside the values rather than in the script
    // for the same reason the values are: a binding is a decision about this user's scene, and the
    // .lua is the brush.
    if (!brush.materialBindings.empty()) {
        nlohmann::json bindings = nlohmann::json::object();
        for (const std::pair<const std::string, std::string>& binding : brush.materialBindings) {
            if (binding.second.empty()) continue;
            bindings[binding.first] = binding.second;
        }
        if (!bindings.empty()) document["materialBindings"] = std::move(bindings);
    }

    std::ofstream file(brush.sidecarPath, std::ios::trunc);
    if (!file.is_open()) {
        core::warn("brush: could not write {}", brush.sidecarPath);
        return false;
    }
    file << document.dump(2) << '\n';
    return file.good();
}

// =============================================================================
// Loading
// =============================================================================

namespace {

// Reads one .lua into a BrushDefinition. `brush.id`, `sourcePath` and `sidecarPath` are already set;
// everything else here is derived from the file, so a failure part-way leaves a definition that is
// listed and explains itself rather than one that is half-loaded.
void loadInto(BrushDefinition& brush) {
    brush.script.reset();
    brush.loadError.clear();
    brush.messages.clear();
    brush.params.clear();
    brush.materials.clear();
    brush.values.clear();
    brush.context = BrushContextMask();
    brush.name = brush.id;
    brush.description.clear();
    brush.author.clear();
    brush.kind = BrushKind::Material;
    brush.maxSkinDepth = 8;
    brush.creviceRadius = 3;
    brush.sourceModifiedTime = fileModifiedTime(brush.sourcePath, brush.sourceSize);

    std::shared_ptr<BrushScript> script = std::make_shared<BrushScript>();
    script->owner = &brush;
    script->state = newSandboxedState(script.get());
    if (!script->state) {
        brush.loadError = "could not create a Lua state (out of memory)";
        return;
    }
    lua_State* state = script->state;

    // The watchdog covers loading too: a script's top level runs arbitrary code, and an endless loop
    // there is exactly as fatal as one in apply.
    script->callStart = std::chrono::steady_clock::now();

    if (luaL_loadfile(state, brush.sourcePath.c_str()) != LUA_OK) {
        brush.loadError = lua_tostring(state, -1) ? lua_tostring(state, -1) : "could not load the file";
        lua_pop(state, 1);
        return;
    }
    if (lua_pcall(state, 0, 1, 0) != LUA_OK) {
        brush.loadError = lua_tostring(state, -1) ? lua_tostring(state, -1) : "the script raised an error";
        lua_pop(state, 1);
        return;
    }
    if (!lua_istable(state, -1)) {
        brush.loadError = "the script must `return` a table describing the brush";
        lua_pop(state, 1);
        return;
    }
    int declaration = lua_gettop(state);

    if (!fieldString(state, declaration, "name", brush.name) || brush.name.empty()) {
        brush.name = brush.id;
    }
    fieldString(state, declaration, "description", brush.description);
    fieldString(state, declaration, "author", brush.author);

    std::string kindName;
    if (!fieldString(state, declaration, "kind", kindName)) {
        brush.loadError = "the brush must declare a `kind` (material, geometry or scatter)";
        lua_pop(state, 1);
        return;
    }
    if (!brushKindFromName(kindName, brush.kind)) {
        brush.loadError = "unknown kind '" + kindName + "' -- expected material, geometry or scatter";
        lua_pop(state, 1);
        return;
    }

    // `needs`, the declared context. A geometry brush always gets Solid whether it asked or not: it
    // is free, it is the one field that kind is about, and a brush that has to remember to ask for it
    // is a brush that will forget.
    lua_getfield(state, declaration, "needs");
    if (lua_istable(state, -1)) {
        lua_Integer count = luaL_len(state, -1);
        for (lua_Integer i = 1; i <= count; i++) {
            lua_rawgeti(state, -1, i);
            if (lua_type(state, -1) == LUA_TSTRING) {
                std::string fieldName = lua_tostring(state, -1);
                BrushContextField field;
                if (brushContextFieldFromName(fieldName, field)) {
                    brush.context.set(field);
                } else {
                    appendMessage(&brush, true, "unknown context field '" + fieldName + "' in `needs`");
                }
            }
            lua_pop(state, 1);
        }
    }
    lua_pop(state, 1);
    if (brush.kind == BrushKind::Geometry) brush.context.set(BrushContextField::Solid);

    double number = 0.0;
    if (fieldNumber(state, declaration, "maxSkinDepth", number)) {
        brush.maxSkinDepth = std::clamp(int(number), 0, 64);
    }
    if (fieldNumber(state, declaration, "creviceRadius", number)) {
        brush.creviceRadius = std::clamp(int(number), 1, 8);
    }

    // `displaces`, the material name prefixes a placement may grow through. See BrushDefinition.
    brush.displaces.clear();
    lua_getfield(state, declaration, "displaces");
    if (lua_istable(state, -1)) {
        lua_Integer count = luaL_len(state, -1);
        for (lua_Integer i = 1; i <= count; i++) {
            lua_rawgeti(state, -1, i);
            if (lua_type(state, -1) == LUA_TSTRING) {
                std::string prefix = lua_tostring(state, -1);
                // An empty prefix matches every name, which would let a placement overwrite the whole
                // scene -- the one value that turns a convenience into a wrecking ball, and easy to
                // arrive at from a stray `""` in a list.
                if (prefix.empty()) {
                    appendMessage(&brush, true,
                                  "an empty prefix in `displaces` would match every material; ignored");
                } else if (brush.kind != BrushKind::Scatter) {
                    appendMessage(&brush, true,
                                  "`displaces` only means anything for a scatter brush; ignored");
                } else {
                    brush.displaces.push_back(std::move(prefix));
                }
            }
            lua_pop(state, 1);
        }
    }
    lua_pop(state, 1);

    lua_getfield(state, declaration, "params");
    if (lua_istable(state, -1)) {
        lua_Integer count = luaL_len(state, -1);
        for (lua_Integer i = 1; i <= count; i++) {
            lua_rawgeti(state, -1, i);
            BrushParam param;
            std::string error;
            if (!readParam(state, param, error)) {
                lua_pop(state, 2);
                brush.loadError = error;
                lua_pop(state, 1);
                return;
            }
            bool duplicate = false;
            for (const BrushParam& existing : brush.params) {
                if (existing.name == param.name) { duplicate = true; break; }
            }
            if (duplicate) {
                lua_pop(state, 2);
                brush.loadError = "two parameters are both called '" + param.name + "'";
                lua_pop(state, 1);
                return;
            }
            brush.params.push_back(std::move(param));
            lua_pop(state, 1);
        }
    }
    lua_pop(state, 1);

    lua_getfield(state, declaration, "materials");
    if (lua_istable(state, -1)) {
        lua_Integer count = luaL_len(state, -1);
        for (lua_Integer i = 1; i <= count; i++) {
            lua_rawgeti(state, -1, i);
            BrushMaterial material;
            std::string error;
            if (!readMaterial(state, material, error)) {
                lua_pop(state, 2);
                brush.loadError = error;
                lua_pop(state, 1);
                return;
            }
            brush.materials.push_back(std::move(material));
            lua_pop(state, 1);
        }
    }
    lua_pop(state, 1);

    // The palette ceiling, stated in the brush's own terms rather than discovered when a stroke runs
    // out of slots. 128 is half a component's palette: a brush wanting more than that is not shading
    // a surface, it is trying to store an image, and the answer is a ramp with fewer steps.
    const int MAX_DECLARED_SLOTS = 128;
    if (brush.totalMaterialSlots() > MAX_DECLARED_SLOTS) {
        brush.loadError = "the brush declares " + std::to_string(brush.totalMaterialSlots()) +
                          " palette entries; the ceiling is " + std::to_string(MAX_DECLARED_SLOTS) +
                          " (material IDs are one byte, and the target's own palette shares the space)";
        lua_pop(state, 1);
        return;
    }
    // A material brush has to have something to write. Counted as *roles* rather than as `materials`
    // entries, because a colour parameter is a role too -- a brush whose whole output is one entry the
    // user assigns from the palette is a perfectly good brush, and the earlier version of this check
    // rejected it for having an empty `materials` list.
    if (brush.kind == BrushKind::Material && brush.totalMaterialSlots() == 0) {
        brush.loadError = "a material brush has to declare something to write: an entry in "
                          "`materials`, or a parameter of type \"color\" for the user to assign";
        lua_pop(state, 1);
        return;
    }

    lua_getfield(state, declaration, "apply");
    if (lua_isfunction(state, -1)) {
        script->applyRef = luaL_ref(state, LUA_REGISTRYINDEX);   // Pops the function.
    } else {
        lua_pop(state, 1);
        if (brushKindIsRunnable(brush.kind)) {
            brush.loadError = "the brush needs an `apply = function(ctx, p) ... end`";
            lua_pop(state, 1);
            return;
        }
    }

    lua_pop(state, 1);   // The declaration table.

    // The context table, created once and reused for every voxel. Building a fresh table per call
    // would put an allocation and a garbage-collector step under every cell of every dab.
    lua_createtable(state, 0, 16);
    script->contextRef = luaL_ref(state, LUA_REGISTRYINDEX);

    brush.values.resize(brush.params.size());
    for (size_t i = 0; i < brush.params.size(); i++) {
        brush.values[i] = defaultValueOf(brush.params[i]);
    }
    loadSidecar(brush);

    brush.script = std::move(script);
}

} // namespace

void brushLibraryLoad(BrushLibrary& library, const std::string& folder, bool create) {
    library.folder = folder;
    library.brushes.clear();
    library.folderError.clear();

    std::error_code code;
    if (!std::filesystem::exists(folder, code)) {
        if (!create) {
            library.folderError = "No brushes folder at " + folder;
            return;
        }
        if (!std::filesystem::create_directories(folder, code) && code) {
            library.folderError = "Could not create " + folder + ": " + code.message();
            return;
        }
    }

    std::vector<std::string> files;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(folder, code)) {
        if (code) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".lua") continue;
        files.push_back(entry.path().string());
    }
    if (code) {
        library.folderError = "Could not read " + folder + ": " + code.message();
        return;
    }
    std::sort(files.begin(), files.end());

    for (const std::string& file : files) {
        auto brush = std::make_shared<BrushDefinition>();
        brush->id = std::filesystem::path(file).stem().string();
        brush->sourcePath = file;
        brush->sidecarPath = sidecarPathFor(file);
        loadInto(*brush);
        if (!brush->loadError.empty()) {
            core::warn("brush '{}': {}", brush->id, brush->loadError);
        }
        library.brushes.push_back(std::move(brush));
    }
    core::info("brushes: {} loaded from {}", library.brushes.size(), folder);
}

bool brushReload(BrushDefinition& brush) {
    // Values are carried across the reload, matched by name *and* type. This is what makes the Lab
    // usable: a script is saved every few seconds while a number is being tuned, and a reload that
    // reset every slider to its default would undo the tuning being done.
    struct Remembered {
        std::string name;
        BrushParamType type;
        BrushParamValue value;
        bool editorDefined;
        BrushParam param;
    };
    std::vector<Remembered> remembered;
    for (size_t i = 0; i < brush.params.size(); i++) {
        Remembered entry;
        entry.name = brush.params[i].name;
        entry.type = brush.params[i].type;
        entry.value = i < brush.values.size() ? brush.values[i] : defaultValueOf(brush.params[i]);
        entry.editorDefined = brush.params[i].editorDefined;
        entry.param = brush.params[i];
        remembered.push_back(std::move(entry));
    }

    loadInto(brush);

    // Editor-defined parameters live in the sidecar, which loadInto has already re-read, so they come
    // back on their own. What has to be restored by hand is the *values*, which the sidecar also holds
    // -- but only as far as the last save. Anything changed since is in `remembered`.
    for (const Remembered& entry : remembered) {
        for (size_t i = 0; i < brush.params.size(); i++) {
            if (brush.params[i].name != entry.name || brush.params[i].type != entry.type) continue;
            BrushParamValue value = entry.value;
            if (brush.params[i].type == BrushParamType::Enum) {
                // The script may have changed the options out from under a stored choice.
                if (std::find(brush.params[i].options.begin(), brush.params[i].options.end(),
                              value.text) == brush.params[i].options.end()) {
                    break;
                }
            }
            if (brush.params[i].type == BrushParamType::Float ||
                brush.params[i].type == BrushParamType::Int ||
                brush.params[i].type == BrushParamType::Seed) {
                value.number = std::clamp(value.number, brush.params[i].minimum,
                                          brush.params[i].maximum);
            }
            brush.values[i] = value;
            break;
        }
    }
    return brush.usable();
}

int brushLibraryPollForChanges(BrushLibrary& library) {
    int reloaded = 0;
    for (std::shared_ptr<BrushDefinition>& brush : library.brushes) {
        size_t size = 0;
        int64_t modified = fileModifiedTime(brush->sourcePath, size);
        // Both, because a filesystem whose timestamp granularity is coarse can save twice within one
        // tick, and an edit that changes a constant usually changes the length too.
        if (modified == brush->sourceModifiedTime && size == brush->sourceSize) continue;
        if (modified == 0 && size == 0) continue;   // Gone, or unreadable: leave the loaded copy alone.
        brushReload(*brush);
        reloaded++;
    }
    return reloaded;
}

std::string brushReadSource(const BrushDefinition& brush) {
    std::ifstream file(brush.sourcePath, std::ios::binary);
    if (!file.is_open()) return std::string();
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

bool brushWriteSource(BrushDefinition& brush, const std::string& source) {
    {
        std::ofstream file(brush.sourcePath, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) return false;
        file << source;
        if (!file.good()) return false;
    }
    return brushReload(brush);
}

BrushDefinition* brushCreate(BrushLibrary& library, const std::string& id, BrushKind kind,
                             const std::string& displayName, std::string& error) {
    if (id.empty()) {
        error = "A brush needs a file name.";
        return nullptr;
    }
    for (char character : id) {
        bool allowed = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                       (character >= '0' && character <= '9') || character == '_' || character == '-';
        if (!allowed) {
            error = "The file name may only hold letters, digits, underscores and hyphens.";
            return nullptr;
        }
    }
    if (library.find(id)) {
        error = "There is already a brush called " + id + ".";
        return nullptr;
    }
    std::error_code code;
    if (!std::filesystem::exists(library.folder, code) &&
        !std::filesystem::create_directories(library.folder, code)) {
        error = "Could not create " + library.folder;
        return nullptr;
    }

    std::filesystem::path path = std::filesystem::path(library.folder) / (id + ".lua");
    if (std::filesystem::exists(path, code)) {
        error = path.string() + " already exists.";
        return nullptr;
    }
    {
        std::ofstream file(path.string(), std::ios::trunc);
        if (!file.is_open()) {
            error = "Could not write " + path.string();
            return nullptr;
        }
        file << brushTemplateSource(kind, displayName.empty() ? id : displayName);
        if (!file.good()) {
            error = "Could not write " + path.string();
            return nullptr;
        }
    }

    auto brush = std::make_shared<BrushDefinition>();
    brush->id = id;
    brush->sourcePath = path.string();
    brush->sidecarPath = sidecarPathFor(path.string());
    loadInto(*brush);
    BrushDefinition* result = brush.get();
    library.brushes.push_back(std::move(brush));
    std::sort(library.brushes.begin(), library.brushes.end(),
              [](const std::shared_ptr<BrushDefinition>& a, const std::shared_ptr<BrushDefinition>& b) {
                  return a->id < b->id;
              });
    return result;
}

BrushDefinition* brushDuplicate(BrushLibrary& library, const BrushDefinition& source,
                                const std::string& newID, std::string& error) {
    if (newID.empty() || library.find(newID)) {
        error = newID.empty() ? "A brush needs a file name."
                              : "There is already a brush called " + newID + ".";
        return nullptr;
    }
    std::string text = brushReadSource(source);
    if (text.empty()) {
        error = "Could not read " + source.sourcePath;
        return nullptr;
    }
    std::filesystem::path path = std::filesystem::path(library.folder) / (newID + ".lua");
    {
        std::ofstream file(path.string(), std::ios::trunc);
        if (!file.is_open()) {
            error = "Could not write " + path.string();
            return nullptr;
        }
        file << text;
    }
    // The sidecar comes too: duplicating a brush that has been tuned and getting the untuned defaults
    // back would make Duplicate useless for the thing it is mostly used for, which is trying a
    // variation of something that already works.
    std::error_code code;
    std::filesystem::copy_file(source.sidecarPath, sidecarPathFor(path.string()),
                               std::filesystem::copy_options::overwrite_existing, code);

    auto brush = std::make_shared<BrushDefinition>();
    brush->id = newID;
    brush->sourcePath = path.string();
    brush->sidecarPath = sidecarPathFor(path.string());
    loadInto(*brush);
    BrushDefinition* result = brush.get();
    library.brushes.push_back(std::move(brush));
    std::sort(library.brushes.begin(), library.brushes.end(),
              [](const std::shared_ptr<BrushDefinition>& a, const std::shared_ptr<BrushDefinition>& b) {
                  return a->id < b->id;
              });
    return result;
}

// =============================================================================
// Parameters
// =============================================================================

void brushResetParam(BrushDefinition& brush, size_t index) {
    if (index >= brush.params.size() || index >= brush.values.size()) return;
    brush.values[index] = defaultValueOf(brush.params[index]);
    brush.valuesDirty = true;
}

void brushResetParams(BrushDefinition& brush) {
    for (size_t i = 0; i < brush.params.size(); i++) brushResetParam(brush, i);
}

bool brushAddEditorParam(BrushDefinition& brush, const BrushParam& param) {
    if (param.name.empty()) return false;
    for (const BrushParam& existing : brush.params) {
        if (existing.name == param.name) return false;
    }
    BrushParam copy = param;
    copy.editorDefined = true;
    if (copy.label.empty()) copy.label = copy.name;
    brush.params.push_back(copy);
    brush.values.push_back(defaultValueOf(copy));
    brush.valuesDirty = true;
    return true;
}

bool brushRemoveEditorParam(BrushDefinition& brush, size_t index) {
    if (index >= brush.params.size() || !brush.params[index].editorDefined) return false;
    brush.params.erase(brush.params.begin() + index);
    if (index < brush.values.size()) brush.values.erase(brush.values.begin() + index);
    brush.valuesDirty = true;
    return true;
}

// =============================================================================
// Evaluation
// =============================================================================

BrushInvocation::~BrushInvocation() = default;
BrushInvocation::BrushInvocation(BrushInvocation&&) noexcept = default;
BrushInvocation& BrushInvocation::operator=(BrushInvocation&&) noexcept = default;

BrushInvocation brushBeginStroke(BrushDefinition& brush, std::string& error) {
    BrushInvocation invocation;
    if (!brush.usable()) {
        error = brush.loadError.empty() ? "The brush is not loaded." : brush.loadError;
        return invocation;
    }
    if (brush.script->applyRef == LUA_NOREF) {
        error = "The brush has no apply function.";
        return invocation;
    }

    lua_State* state = brush.script->state;

    // The parameter table, built once for the stroke. Values cannot change mid-stroke, which is not a
    // limitation to work around but the rule: a dab whose settings changed half way through is not
    // reproducible, and reproducibility is what lets the same stroke be previewed and then applied.
    if (brush.script->paramsRef != LUA_NOREF) {
        luaL_unref(state, LUA_REGISTRYINDEX, brush.script->paramsRef);
        brush.script->paramsRef = LUA_NOREF;
    }
    // Resolved once for the whole table: a colour parameter needs to know which material index it
    // occupies, and the expansion is the only thing that knows the ordering.
    std::vector<BrushMaterialSlot> slots;
    brushExpandMaterials(brush, slots);

    lua_createtable(state, 0, int(brush.params.size()));
    for (size_t i = 0; i < brush.params.size(); i++) {
        const BrushParam& param = brush.params[i];
        const BrushParamValue& value = i < brush.values.size() ? brush.values[i]
                                                               : defaultValueOf(param);
        switch (param.type) {
            case BrushParamType::Float:
                lua_pushnumber(state, value.number);
                break;
            case BrushParamType::Int:
            case BrushParamType::Seed:
                lua_pushinteger(state, lua_Integer(std::llround(value.number)));
                break;
            case BrushParamType::Bool:
                lua_pushboolean(state, value.boolean ? 1 : 0);
                break;
            case BrushParamType::Color: {
                // As a table of three, and also as .r/.g/.b: a colour is read both ways depending on
                // what the script is doing with it, and neither spelling is obviously the right one.
                lua_createtable(state, 3, 5);
                static const char* const CHANNELS[3] = { "r", "g", "b" };
                for (int channel = 0; channel < 3; channel++) {
                    lua_pushnumber(state, value.color[channel]);
                    lua_rawseti(state, -2, channel + 1);
                    lua_pushnumber(state, value.color[channel]);
                    lua_setfield(state, -2, CHANNELS[channel]);
                }
                // ...and `.index`, which is the field that makes a colour setting useful rather than
                // decorative. A brush returns a material index, so without this a script could read
                // the colour the user chose and had no way to paint with it -- it would have to
                // declare a material as well and hope the two agreed. `return p.tint.index` is the
                // whole gesture. `.name` is the palette entry it resolved to, for a message or a
                // comparison against ctx.material.
                {
                    int slotIndex = 0;
                    for (size_t s = 0; s < slots.size(); s++) {
                        if (slots[s].paramIndex == int(i)) { slotIndex = int(s) + 1; break; }
                    }
                    lua_pushinteger(state, slotIndex);
                    lua_setfield(state, -2, "index");
                    lua_pushstring(state, slotIndex > 0 ? slots[size_t(slotIndex - 1)].name.c_str() : "");
                    lua_setfield(state, -2, "name");
                }
                break;
            }
            case BrushParamType::Enum:
            case BrushParamType::Text:
            case BrushParamType::Asset:
                lua_pushstring(state, value.text.c_str());
                break;
        }
        lua_setfield(state, -2, param.name.c_str());
    }
    brush.script->paramsRef = luaL_ref(state, LUA_REGISTRYINDEX);

    invocation.definition = &brush;
    invocation.script = brush.script;
    invocation.slotCount = brush.totalMaterialSlots();
    return invocation;
}

namespace {

// The context table is on top of the stack.
void pushNumberField(lua_State* state, const char* key, double value) {
    lua_pushnumber(state, value);
    lua_setfield(state, -2, key);
}

void pushIntegerField(lua_State* state, const char* key, lua_Integer value) {
    lua_pushinteger(state, value);
    lua_setfield(state, -2, key);
}

} // namespace

// Writes the declared context fields into the reused table, which is on top of the stack.
//
// Only the declared ones. A brush that asks for nothing gets an empty table, which is both correct and
// the cheapest call this can make -- and the declaration is what stops a brush paying for a field it
// never reads, per voxel, forever.
static void pushContextFields(lua_State* state, const BrushDefinition& brush,
                              const BrushContext& context) {
    if (brush.context.has(BrushContextField::Position)) {
        pushIntegerField(state, "x", context.x);
        pushIntegerField(state, "y", context.y);
        pushIntegerField(state, "z", context.z);
    }
    if (brush.context.has(BrushContextField::World)) {
        pushNumberField(state, "wx", context.wx);
        pushNumberField(state, "wy", context.wy);
        pushNumberField(state, "wz", context.wz);
    }
    if (brush.context.has(BrushContextField::Solid)) {
        lua_pushboolean(state, context.solid ? 1 : 0);
        lua_setfield(state, -2, "solid");
    }
    if (brush.context.has(BrushContextField::Material)) {
        pushIntegerField(state, "slot", context.slot);
        pushNumberField(state, "r", context.r);
        pushNumberField(state, "g", context.g);
        pushNumberField(state, "b", context.b);
        if (context.materialName) lua_pushstring(state, context.materialName);
        else lua_pushnil(state);
        lua_setfield(state, -2, "material");
    }
    if (brush.context.has(BrushContextField::SkinDepth)) {
        pushIntegerField(state, "depth", context.depth);
    }
    if (brush.context.has(BrushContextField::Crevice)) {
        pushNumberField(state, "crevice", context.crevice);
    }
    if (brush.context.has(BrushContextField::Distance)) {
        pushNumberField(state, "distance", context.distance);
    }
    if (brush.context.has(BrushContextField::Normal)) {
        pushNumberField(state, "nx", context.nx);
        pushNumberField(state, "ny", context.ny);
        pushNumberField(state, "nz", context.nz);
    }
}

BrushVerdict brushEvaluate(BrushInvocation& invocation, const BrushContext& context) {
    BrushVerdict verdict;
    if (!invocation.valid() || invocation.failedFlag) return verdict;

    BrushScript* script = invocation.script.get();
    lua_State* state = script->state;
    const BrushDefinition& brush = *invocation.definition;

    lua_rawgeti(state, LUA_REGISTRYINDEX, script->applyRef);
    lua_rawgeti(state, LUA_REGISTRYINDEX, script->contextRef);
    pushContextFields(state, brush, context);
    lua_rawgeti(state, LUA_REGISTRYINDEX, script->paramsRef);

    script->callStart = std::chrono::steady_clock::now();
    invocation.calls++;
    if (lua_pcall(state, 2, 1, 0) != LUA_OK) {
        const char* message = lua_tostring(state, -1);
        invocation.failedFlag = true;
        invocation.errorText = message ? message : "the brush raised an error";
        lua_pop(state, 1);
        // Reported once per stroke, not once per voxel: the caller stops asking as soon as it sees
        // failed(), and the message it puts in the log is this one.
        appendMessage(script->owner, true, invocation.errorText);
        return verdict;
    }

    // nil / false -> leave it alone. false is spelled out as Erase for a geometry brush, which is the
    // one place the two answers differ, and the reason a material brush cannot carve: there is no
    // return value it could give that would mean "remove this voxel".
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return verdict;
    }
    if (lua_isboolean(state, -1)) {
        bool value = lua_toboolean(state, -1) != 0;
        lua_pop(state, 1);
        if (!value && brush.kind == BrushKind::Geometry) verdict.action = BrushAction::Erase;
        return verdict;
    }
    if (!lua_isnumber(state, -1)) {
        invocation.failedFlag = true;
        invocation.errorText = std::string("apply returned a ") + luaL_typename(state, -1) +
                               "; expected a material index, false, or nil";
        lua_pop(state, 1);
        appendMessage(script->owner, true, invocation.errorText);
        return verdict;
    }

    // One-based, because it indexes the `materials` list the script itself wrote and Lua lists are
    // one-based. Returning 0 is not an error and does not mean the first entry: it means nothing,
    // which is the same as nil -- a brush computing `n` and finding none is a normal outcome.
    lua_Integer index = lua_tointeger(state, -1);
    lua_pop(state, 1);
    if (index <= 0) return verdict;

    if (index > lua_Integer(invocation.slotCount)) {
        invocation.failedFlag = true;
        invocation.errorText = "apply returned material index " + std::to_string(index) +
                               ", but the brush declares " + std::to_string(invocation.slotCount) +
                               " palette " + (invocation.slotCount == 1 ? "entry" : "entries");
        appendMessage(script->owner, true, invocation.errorText);
        return verdict;
    }
    verdict.action = BrushAction::Write;
    verdict.materialIndex = int(index) - 1;
    return verdict;
}

void brushSetSolidQuery(BrushDefinition& brush, BrushSolidQuery query) {
    if (!brush.script) return;
    brush.script->solidQuery = std::move(query);
}

namespace {

// One entry of a placement list. Accepts both spellings a script might reasonably use:
//
//     { 0, 1, 0, 2 }                          -- positional, which is what a generated list looks like
//     { x = 0, y = 1, z = 0, material = 2 }   -- named, which is what a hand-written one looks like
//
// Supporting both costs four lookups on a table that is already on the stack, and the alternative is
// a brush author reading the documentation to find out which of two obvious spellings was chosen.
bool readPlacementVoxel(lua_State* state, BrushPlacementVoxel& voxel) {
    if (!lua_istable(state, -1)) return false;

    bool positional = false;
    for (int i = 0; i < 4; i++) {
        lua_rawgeti(state, -1, i + 1);
        bool present = lua_isnumber(state, -1) != 0;
        if (present) {
            lua_Integer value = lua_tointeger(state, -1);
            switch (i) {
                case 0: voxel.dx = int32_t(value); break;
                case 1: voxel.dy = int32_t(value); break;
                case 2: voxel.dz = int32_t(value); break;
                default: voxel.materialIndex = int(value) - 1; break;   // 1-based on the Lua side.
            }
            if (i < 3) positional = true;
        }
        lua_pop(state, 1);
    }
    if (positional) return true;

    static const char* const KEYS[4] = { "x", "y", "z", "material" };
    bool named = false;
    for (int i = 0; i < 4; i++) {
        lua_getfield(state, -1, KEYS[i]);
        if (lua_isnumber(state, -1)) {
            lua_Integer value = lua_tointeger(state, -1);
            switch (i) {
                case 0: voxel.dx = int32_t(value); named = true; break;
                case 1: voxel.dy = int32_t(value); named = true; break;
                case 2: voxel.dz = int32_t(value); named = true; break;
                default: voxel.materialIndex = int(value) - 1; break;
            }
        }
        lua_pop(state, 1);
    }
    return named;
}

} // namespace

bool brushEvaluateScatter(BrushInvocation& invocation, const BrushContext& context,
                          std::vector<BrushPlacementVoxel>& out) {
    out.clear();
    if (!invocation.valid() || invocation.failedFlag) return false;

    BrushScript* script = invocation.script.get();
    lua_State* state = script->state;
    const BrushDefinition& brush = *invocation.definition;

    lua_rawgeti(state, LUA_REGISTRYINDEX, script->applyRef);
    lua_rawgeti(state, LUA_REGISTRYINDEX, script->contextRef);
    pushContextFields(state, brush, context);
    lua_rawgeti(state, LUA_REGISTRYINDEX, script->paramsRef);

    script->callStart = std::chrono::steady_clock::now();
    invocation.calls++;
    if (lua_pcall(state, 2, 1, 0) != LUA_OK) {
        const char* message = lua_tostring(state, -1);
        invocation.failedFlag = true;
        invocation.errorText = message ? message : "the brush raised an error";
        lua_pop(state, 1);
        appendMessage(script->owner, true, invocation.errorText);
        return false;
    }

    // nil, false, or an empty list: nothing grows here. The overwhelmingly common answer, and not
    // something to report -- a scatter brush is asked about every eligible site and plants on few.
    if (lua_isnil(state, -1) || lua_isboolean(state, -1)) {
        lua_pop(state, 1);
        return false;
    }
    if (!lua_istable(state, -1)) {
        invocation.failedFlag = true;
        invocation.errorText = std::string("a scatter brush's apply returned a ") +
                               luaL_typename(state, -1) +
                               "; expected a list of voxels, or nil to plant nothing";
        lua_pop(state, 1);
        appendMessage(script->owner, true, invocation.errorText);
        return false;
    }

    lua_Integer count = luaL_len(state, -1);
    // A placement is an object, not a landscape. The cap is what stops one runaway loop in a script
    // from queueing a million voxels per site across a dab of hundreds of sites.
    //
    // **Sized for the largest honest object, not for the smallest.** At 20000 this was a limit on what
    // could be modelled rather than a guard against a bug: a tree 200 voxels tall needs a trunk with a
    // radius near 20, and the *shell* of that trunk alone is past 20000 before a leaf is placed. So a
    // brush working at a high voxel resolution could not describe its subject at all, and the failure
    // arrived as a refused placement rather than as anything a reader would connect to a ceiling.
    //
    // What the cap actually has to catch is the runaway -- the `while true do` that queues until
    // memory runs out -- and a million is the number that comment has always named. This stays an
    // order of magnitude below it while leaving room for a large object, and nothing else is spending
    // its own budget on the strength of this one: a placement is refused whole, so the cost of the
    // ceiling being generous is a slower dab, never a corrupted one. The scatter lattice bounds how
    // many placements a dab can hold, and the fit test bounds how many can overlap (none).
    const lua_Integer MAX_PLACEMENT_VOXELS = 120000;
    if (count > MAX_PLACEMENT_VOXELS) {
        invocation.failedFlag = true;
        invocation.errorText = "one placement returned " + std::to_string(count) +
                               " voxels; the ceiling is " + std::to_string(MAX_PLACEMENT_VOXELS);
        lua_pop(state, 1);
        appendMessage(script->owner, true, invocation.errorText);
        return false;
    }

    out.reserve(size_t(count));
    for (lua_Integer i = 1; i <= count; i++) {
        lua_rawgeti(state, -1, i);
        BrushPlacementVoxel voxel;
        bool ok = readPlacementVoxel(state, voxel);
        lua_pop(state, 1);
        if (!ok) continue;
        if (voxel.materialIndex < 0) voxel.materialIndex = 0;
        if (voxel.materialIndex >= invocation.slotCount) {
            invocation.failedFlag = true;
            invocation.errorText = "a placement voxel asked for material " +
                                   std::to_string(voxel.materialIndex + 1) + ", but the brush declares " +
                                   std::to_string(invocation.slotCount);
            lua_pop(state, 1);
            appendMessage(script->owner, true, invocation.errorText);
            out.clear();
            return false;
        }
        out.push_back(voxel);
    }
    lua_pop(state, 1);
    return !out.empty();
}

// =============================================================================
// Templates
// =============================================================================
//
// What New Brush writes. Each one runs as-is and does something visible, because the first thing
// anyone does with a new brush is preview it, and a template that needed editing before it worked
// would make the first experience of the Lab an error message.

std::string brushTemplateSource(BrushKind kind, const std::string& displayName) {
    std::string name = displayName.empty() ? std::string("New Brush") : displayName;
    std::string quoted;
    for (char character : name) {
        if (character == '"' || character == '\\') quoted += '\\';
        quoted += character;
    }

    if (kind == BrushKind::Geometry) {
        return
"-- " + name + " -- a geometry brush: it decides which cells are solid.\n"
"--\n"
"-- apply is called once per cell of the dab, empty ones included. Return a material index to fill\n"
"-- the cell, false to empty it, or nil to leave it exactly as it is.\n"
"return {\n"
"    name = \"" + quoted + "\",\n"
"    kind = \"geometry\",\n"
"    description = \"Roughens a surface with noise.\",\n"
"\n"
"    -- Only what is listed here is computed and handed to apply.\n"
"    needs = { \"position\", \"solid\", \"distance\" },\n"
"\n"
"    params = {\n"
"        { name = \"scale\", label = \"Feature size\", type = \"float\",\n"
"          default = 0.25, min = 0.02, max = 1.0,\n"
"          tooltip = \"Noise frequency, in voxels^-1. Smaller is chunkier.\" },\n"
"        { name = \"bias\", label = \"Fill\", type = \"float\", default = 0.5, min = 0.0, max = 1.0,\n"
"          tooltip = \"Above this the cell is filled. 0 fills everything, 1 fills nothing.\" },\n"
"        { name = \"seed\", type = \"seed\", default = 1 },\n"
"    },\n"
"\n"
"    materials = {\n"
"        { name = \"rough\", color = { 0.62, 0.60, 0.56 } },\n"
"    },\n"
"\n"
"    apply = function(ctx, p)\n"
"        -- Falls off towards the rim of the brush, so a dab has a soft edge rather than a hard ball.\n"
"        local n = pv.noise(ctx.x * p.scale, ctx.y * p.scale, ctx.z * p.scale, p.seed)\n"
"        n = n - ctx.distance * 0.5\n"
"        if n > p.bias then return 1 end\n"
"        return nil\n"
"    end,\n"
"}\n";
    }

    if (kind == BrushKind::Scatter) {
        return
"-- " + name + " -- a scatter brush: it plants objects on the surface.\n"
"--\n"
"-- Asked about *sites*, not cells. The editor picks them -- one per cell of a lattice fixed to the\n"
"-- model, so a second pass over the same ground plants nothing new -- and apply answers with the\n"
"-- object to grow there, as a list of voxels offset from the site:\n"
"--\n"
"--     return { {0,1,0, 1}, {0,2,0, 1} }     -- dx, dy, dz, material\n"
"--\n"
"-- ...or nil to grow nothing, which is the ordinary answer.\n"
"--\n"
"-- The whole placement is checked for room before any of it lands: if one voxel of it would grow\n"
"-- through something, none of it is planted. Ask about anything else with pv.solid(x,y,z) or\n"
"-- pv.fits(x0,y0,z0, x1,y1,z1).\n"
"return {\n"
"    name = \"" + quoted + "\",\n"
"    kind = \"scatter\",\n"
"    description = \"Plants little posts on flat, open ground.\",\n"
"\n"
"    needs = { \"position\", \"normal\" },\n"
"\n"
"    params = {\n"
"        -- `spacing` and `density` are read by the editor, by name: they decide where the sites are\n"
"        -- before this script is asked about any of them.\n"
"        { name = \"spacing\", label = \"Spacing\", type = \"int\", default = 3, min = 1, max = 32,\n"
"          tooltip = \"How far apart sites are, in voxels.\" },\n"
"        { name = \"density\", label = \"Coverage\", type = \"float\",\n"
"          default = 0.5, min = 0.0, max = 1.0,\n"
"          tooltip = \"What fraction of those sites are taken.\" },\n"
"        { name = \"height\", label = \"Height\", type = \"int\", default = 4, min = 1, max = 24 },\n"
"        { name = \"maxSlope\", label = \"Steepest ground\", type = \"float\",\n"
"          default = 0.5, min = 0.0, max = 1.0 },\n"
"        { name = \"seed\", type = \"seed\", default = 1 },\n"
"    },\n"
"\n"
"    materials = {\n"
"        { name = \"post\", color = { 0.62, 0.48, 0.28 } },\n"
"        { name = \"tip\", color = { 0.92, 0.90, 0.62 } },\n"
"    },\n"
"\n"
"    apply = function(ctx, p)\n"
"        -- Only on ground facing up enough. ctx.ny is 1 on the level and 0 on a vertical wall.\n"
"        if ctx.ny < 1.0 - p.maxSlope then return nil end\n"
"\n"
"        -- A pure function of the site's own coordinate, so the same post grows in the same place\n"
"        -- every time -- which is what makes a preview a preview.\n"
"        local height = 1 + math.floor(pv.hash(ctx.x, ctx.y, ctx.z, p.seed) * p.height)\n"
"\n"
"        -- Room to grow? The editor checks the placement itself, but a brush can look further ahead:\n"
"        -- here, that nothing is hanging directly over the site.\n"
"        if not pv.fits(ctx.x, ctx.y + 1, ctx.z, ctx.x, ctx.y + height + 1, ctx.z) then return nil end\n"
"\n"
"        local voxels = {}\n"
"        for i = 1, height do voxels[#voxels + 1] = { 0, i, 0, 1 } end\n"
"        voxels[#voxels + 1] = { 0, height + 1, 0, 2 }\n"
"        return voxels\n"
"    end,\n"
"}\n";
    }

    return
"-- " + name + " -- a material brush: it recolours voxels that already exist.\n"
"--\n"
"-- apply is called once per solid voxel of the dab. Return the index of one of the materials below\n"
"-- to paint the voxel that colour, or nil to leave it alone. It cannot add or remove geometry.\n"
"return {\n"
"    name = \"" + quoted + "\",\n"
"    kind = \"material\",\n"
"    description = \"Darkens what is buried and lightens what is exposed.\",\n"
"\n"
"    -- Only what is listed here is computed and handed to apply. skinDepth and crevice both cost a\n"
"    -- margin around the brush, so ask for them when you use them and not otherwise.\n"
"    needs = { \"position\", \"skinDepth\", \"crevice\" },\n"
"    maxSkinDepth = 4,\n"
"    creviceRadius = 3,\n"
"\n"
"    params = {\n"
"        { name = \"scale\", label = \"Grain size\", type = \"float\",\n"
"          default = 0.2, min = 0.02, max = 1.0,\n"
"          tooltip = \"Noise frequency, in voxels^-1.\" },\n"
"        { name = \"shade\", label = \"Crevice darkening\", type = \"float\",\n"
"          default = 0.7, min = 0.0, max = 1.0 },\n"
"        { name = \"seed\", type = \"seed\", default = 1 },\n"
"    },\n"
"\n"
"    -- A ramp: `steps` entries interpolated from color to colorTo. Returning an index into it is how\n"
"    -- a brush shades without spending the palette.\n"
"    --\n"
"    -- The colours here are only what the entries are given **if they have to be created**. Once they\n"
"    -- exist they belong to the component's palette, and the Palette panel along the bottom is where\n"
"    -- they are recoloured, renamed and removed. Any of these roles can also be pointed at an entry\n"
"    -- that already exists: click its swatch under \"What it writes\", then click the entry.\n"
"    materials = {\n"
"        { name = \"" + quoted + "\", steps = 8,\n"
"          color = { 0.68, 0.66, 0.62 }, colorTo = { 0.16, 0.15, 0.14 } },\n"
"    },\n"
"\n"
"    apply = function(ctx, p)\n"
"        local grain = pv.fbm(ctx.x * p.scale, ctx.y * p.scale, ctx.z * p.scale, 3, p.seed)\n"
"        -- Buried voxels and tight creases go darker; exposed faces keep the grain.\n"
"        local dark = ctx.crevice * p.shade + grain * 0.4\n"
"        if ctx.depth > 0 then dark = dark + 0.25 end\n"
"        return 1 + math.floor(pv.clamp(dark, 0.0, 0.999) * 8)\n"
"    end,\n"
"}\n";
}

} // namespace projv::editor
