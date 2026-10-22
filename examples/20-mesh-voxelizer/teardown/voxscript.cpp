// Teardown voxscripts: Lua that draws voxels when the level loads -- trees, windows, roofs, walls,
// terrain from heightmaps. Levels lean on them heavily (one mod has six hundred pine trees), so
// they are run here, against an implementation of the voxscript API in data/voxscript_defs.lua.
//
// Each Vox() call starts a shape placed relative to the voxscript node; drawing calls work in that
// shape's voxel coordinates. Material handles are small integers in creation order (ground.lua's
// Heightmap depends on that order); brush handles live above BRUSH_BASE.

#ifdef PROJV_TEARDOWN_LUA

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <random>
#include <sstream>
#include <unordered_map>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include "png.h"
#include "pugixml.hpp"
#include "shapes.h"
#include "vox_file.h"

namespace fs = std::filesystem;

namespace teardown {

namespace {
    constexpr int BRUSH_BASE = 1 << 20;

    struct ScriptBrush {
        Brush brush;
        bool local = false;
    };

    struct Run {
        pugi::xml_node params;
        const ShapeContext* ctx = nullptr;
        std::string scriptDir;
        std::vector<LocalShape>* out = nullptr;
        LocalShape* cur = nullptr;
        std::vector<MatId> handles{MAT_EMPTY};          // handle -> material
        std::unordered_map<MatId, int> handleOf;
        std::vector<ScriptBrush> brushes;
        int material = 0;
        std::mt19937 rng{1};
        Image image, grass;
        bool hasGrass = false;

        std::string resolve(const std::string& p) const {
            std::string r = ctx->paths.resolve(p, scriptDir);
            return r.empty() ? ctx->paths.resolve(p, ctx->xmlDir) : r;
        }

        LocalShape& shape() {
            if (!cur) { out->emplace_back(); cur = &out->back(); }
            return *cur;
        }

        int handleFor(MatId m) {
            if (m == MAT_EMPTY) return 0;
            auto it = handleOf.find(m);
            if (it != handleOf.end()) return it->second;
            handles.push_back(m);
            return handleOf[m] = int(handles.size() - 1);
        }

        // The current material at a voxel. `boxMin` is where a local brush starts.
        MatId sample(ivec3 p, ivec3 boxMin) const {
            if (material >= BRUSH_BASE) {
                const ScriptBrush& b = brushes[size_t(material - BRUSH_BASE)];
                const ivec3 q = b.local ? p - boxMin : p;
                return b.brush.sample(q.x, q.y, q.z); // empty brush voxels draw nothing (see put)
            }
            return material > 0 && size_t(material) < handles.size() ? handles[size_t(material)] : MAT_EMPTY;
        }
        bool drawsNothing(MatId m) const { return m == MAT_EMPTY && material >= BRUSH_BASE; }

        void put(ivec3 p, ivec3 boxMin) {
            MatId m = sample(p, boxMin);
            if (drawsNothing(m)) return;
            shape().voxels.set(p.x, p.y, p.z, m);
        }
    };

    Run* run(lua_State* L) { return static_cast<Run*>(lua_touserdata(L, lua_upvalueindex(1))); }

    int roundi(double v) { return int(std::floor(v + 0.5)); }

    // Parameters: <parameters name="a b c"/> beside the script; missing values take the defaults
    // given to the getter, value by value.
    int getNumbers(lua_State* L, bool integer) {
        Run* r = run(L);
        const char* name = luaL_checkstring(L, 1);
        const int defaults = lua_gettop(L) - 1;
        std::vector<double> vals;
        if (pugi::xml_attribute a = r->params.attribute(name)) {
            std::stringstream ss(a.as_string());
            double v;
            while (ss >> v) vals.push_back(v);
        }
        const int n = std::max(defaults, int(vals.size()));
        for (int i = 0; i < std::max(n, 1); i++) {
            double v = i < int(vals.size()) ? vals[size_t(i)] : (i < defaults ? lua_tonumber(L, 2 + i) : 0.0);
            if (integer) lua_pushinteger(L, lua_Integer(std::llround(v)));
            else lua_pushnumber(L, v);
        }
        return std::max(n, 1);
    }
    int l_GetFloat(lua_State* L) { return getNumbers(L, false); }
    int l_GetInt(lua_State* L) { return getNumbers(L, true); }
    int l_GetColor(lua_State* L) { return getNumbers(L, false); }
    int l_GetBool(lua_State* L) {
        Run* r = run(L);
        pugi::xml_attribute a = r->params.attribute(luaL_checkstring(L, 1));
        if (!a) lua_pushboolean(L, lua_toboolean(L, 2));
        else lua_pushboolean(L, std::strcmp(a.as_string(), "true") == 0 || std::strcmp(a.as_string(), "1") == 0);
        return 1;
    }
    int l_GetString(lua_State* L) {
        Run* r = run(L);
        pugi::xml_attribute a = r->params.attribute(luaL_checkstring(L, 1));
        if (a) lua_pushstring(L, a.as_string());
        else lua_pushstring(L, luaL_optstring(L, 2, ""));
        return 1;
    }

    int l_Randomize(lua_State* L) {
        run(L)->rng.seed(uint32_t(luaL_optinteger(L, 1, 1)));
        return 0;
    }
    int l_RandomFloat(lua_State* L) {
        double lo = luaL_checknumber(L, 1), hi = luaL_checknumber(L, 2);
        lua_pushnumber(L, lo + (hi - lo) * std::uniform_real_distribution<double>(0.0, 1.0)(run(L)->rng));
        return 1;
    }
    int l_RandomInt(lua_State* L) {
        lua_Integer lo = luaL_checkinteger(L, 1), hi = luaL_checkinteger(L, 2);
        if (hi < lo) std::swap(lo, hi);
        lua_pushinteger(L, std::uniform_int_distribution<lua_Integer>(lo, hi)(run(L)->rng));
        return 1;
    }

    int l_CreateMaterial(lua_State* L) {
        Run* r = run(L);
        VoxMaterial m = makeMaterial(substanceFromName(luaL_checkstring(L, 1)), float(luaL_optnumber(L, 2, 1)),
                                     float(luaL_optnumber(L, 3, 1)), float(luaL_optnumber(L, 4, 1)),
                                     float(luaL_optnumber(L, 5, 1)), float(luaL_optnumber(L, 6, 0)),
                                     float(luaL_optnumber(L, 7, 0)), float(luaL_optnumber(L, 8, 0)),
                                     float(luaL_optnumber(L, 9, 0)));
        // Always a new handle, even for a duplicate: scripts count on creation order.
        r->handles.push_back(materials().intern(m));
        lua_pushinteger(L, lua_Integer(r->handles.size() - 1));
        return 1;
    }

    int l_CreateBrush(lua_State* L) {
        Run* r = run(L);
        std::string spec = luaL_checkstring(L, 1);
        std::string object;
        // "file.vox:object" -- the colon after the extension, not a drive letter.
        if (size_t c = spec.rfind(':'); c != std::string::npos && c > spec.rfind(".vox")) {
            object = spec.substr(c + 1);
            spec = spec.substr(0, c);
        }
        ScriptBrush b;
        b.local = lua_toboolean(L, 2);
        const std::string path = r->resolve(spec);
        if (path.empty() || !makeBrush(path, object, b.brush)) {
            logWarn("voxscript: brush '%s' not found", spec.c_str());
        }
        r->brushes.push_back(std::move(b));
        lua_pushinteger(L, BRUSH_BASE + lua_Integer(r->brushes.size() - 1));
        return 1;
    }

    ScriptBrush* brushArg(lua_State* L, int idx) {
        Run* r = run(L);
        lua_Integer h = luaL_checkinteger(L, idx) - BRUSH_BASE;
        return h >= 0 && size_t(h) < r->brushes.size() ? &r->brushes[size_t(h)] : nullptr;
    }

    int l_FlipBrush(lua_State* L) {
        if (ScriptBrush* b = brushArg(L, 1)) {
            const std::string axes = luaL_checkstring(L, 2);
            glm::imat3x3 f(1);
            if (axes.find('x') != std::string::npos) f[0][0] = -1;
            if (axes.find('y') != std::string::npos) f[1][1] = -1;
            if (axes.find('z') != std::string::npos) f[2][2] = -1;
            b->brush.xform = b->brush.xform * f;
        }
        return 0;
    }
    int l_RotateBrush(lua_State* L) {
        if (ScriptBrush* b = brushArg(L, 1)) {
            const std::string axis = luaL_checkstring(L, 2);
            int steps = floorMod(roundi(luaL_checknumber(L, 3) / 90.0), 4);
            // A quarter turn of sample coordinates about the axis, `steps` times.
            glm::imat3x3 q(1);
            if (axis == "x") q = glm::imat3x3(1, 0, 0, 0, 0, 1, 0, -1, 0);
            else if (axis == "y") q = glm::imat3x3(0, 0, -1, 0, 1, 0, 1, 0, 0);
            else q = glm::imat3x3(0, 1, 0, -1, 0, 0, 0, 0, 1);
            for (int i = 0; i < steps; i++) b->brush.xform = b->brush.xform * q;
        }
        return 0;
    }
    int l_TranslateBrush(lua_State* L) {
        if (ScriptBrush* b = brushArg(L, 1))
            b->brush.offset -= ivec3(roundi(luaL_checknumber(L, 2)), roundi(luaL_checknumber(L, 3)), roundi(luaL_checknumber(L, 4)));
        return 0;
    }
    int l_GetBrushSize(lua_State* L) {
        ScriptBrush* b = brushArg(L, 1);
        ivec3 s = b ? b->brush.size : ivec3(0);
        lua_pushinteger(L, s.x);
        lua_pushinteger(L, s.y);
        lua_pushinteger(L, s.z);
        return 3;
    }

    int l_Material(lua_State* L) {
        run(L)->material = int(luaL_optinteger(L, 1, 0));
        return 0;
    }

    int l_LoadImage(lua_State* L) {
        Run* r = run(L);
        const std::string path = r->resolve(luaL_checkstring(L, 1));
        if (path.empty() || !loadImage(path, r->image)) logWarn("voxscript: image '%s' not found", luaL_checkstring(L, 1));
        r->hasGrass = false;
        if (lua_gettop(L) >= 2 && lua_isstring(L, 2) && std::strlen(lua_tostring(L, 2)) > 0) {
            const std::string gpath = r->resolve(lua_tostring(L, 2));
            r->hasGrass = !gpath.empty() && loadImage(gpath, r->grass);
        }
        return 0;
    }
    int l_GetImageSize(lua_State* L) {
        Run* r = run(L);
        lua_pushinteger(L, r->image.w);
        lua_pushinteger(L, r->image.h);
        return 2;
    }
    int l_GetImagePixel(lua_State* L) {
        Run* r = run(L);
        int x = int(luaL_checkinteger(L, 1)), y = int(luaL_checkinteger(L, 2));
        if (x < 0 || y < 0 || x >= r->image.w || y >= r->image.h) {
            for (int i = 0; i < 4; i++) lua_pushnumber(L, 0);
            return 4;
        }
        const uint8_t* p = r->image.at(x, y);
        for (int i = 0; i < 4; i++) lua_pushnumber(L, p[i] / 255.0);
        return 4;
    }

    int l_Vox(lua_State* L) {
        Run* r = run(L);
        r->out->emplace_back();
        r->cur = &r->out->back();
        vec3 p(float(luaL_optnumber(L, 1, 0)), float(luaL_optnumber(L, 2, 0)), float(luaL_optnumber(L, 3, 0)));
        const bool roundPos = lua_gettop(L) < 7 || lua_toboolean(L, 7);
        if (roundPos) p = glm::floor(p + 0.5f);
        r->cur->local.pos = p * TD_VOXEL;
        r->cur->local.rot = quatEuler(float(luaL_optnumber(L, 4, 0)), float(luaL_optnumber(L, 5, 0)), float(luaL_optnumber(L, 6, 0)));
        return 0;
    }

    int l_Box(lua_State* L) {
        Run* r = run(L);
        vec3 a(float(luaL_checknumber(L, 1)), float(luaL_checknumber(L, 2)), float(luaL_checknumber(L, 3)));
        vec3 b(float(luaL_checknumber(L, 4)), float(luaL_checknumber(L, 5)), float(luaL_checknumber(L, 6)));
        ivec3 lo(glm::floor(glm::min(a, b) + 0.5f)), hi(glm::floor(glm::max(a, b) + 0.5f));
        for (int z = lo.z; z < hi.z; z++)
            for (int y = lo.y; y < hi.y; y++)
                for (int x = lo.x; x < hi.x; x++) r->put(ivec3(x, y, z), lo);
        return 0;
    }

    int l_Sphere(lua_State* L) {
        Run* r = run(L);
        vec3 c(float(luaL_checknumber(L, 1)), float(luaL_checknumber(L, 2)), float(luaL_checknumber(L, 3)));
        float rad = float(luaL_checknumber(L, 4));
        ivec3 lo(glm::floor(c - rad)), hi(glm::ceil(c + rad));
        for (int z = lo.z; z <= hi.z; z++)
            for (int y = lo.y; y <= hi.y; y++)
                for (int x = lo.x; x <= hi.x; x++)
                    if (glm::length(vec3(x, y, z) + 0.5f - c) <= rad) r->put(ivec3(x, y, z), lo);
        return 0;
    }

    int l_Line(lua_State* L) {
        Run* r = run(L);
        vec3 a(float(luaL_checknumber(L, 1)), float(luaL_checknumber(L, 2)), float(luaL_checknumber(L, 3)));
        vec3 b(float(luaL_checknumber(L, 4)), float(luaL_checknumber(L, 5)), float(luaL_checknumber(L, 6)));
        float r0 = float(luaL_optnumber(L, 7, 0)), r1 = float(luaL_optnumber(L, 8, r0));
        const float rmax = std::max({r0, r1, 0.5f});
        ivec3 lo(glm::floor(glm::min(a, b) - rmax)), hi(glm::ceil(glm::max(a, b) + rmax));
        const vec3 d = b - a;
        const float len2 = glm::dot(d, d);
        for (int z = lo.z; z <= hi.z; z++)
            for (int y = lo.y; y <= hi.y; y++)
                for (int x = lo.x; x <= hi.x; x++) {
                    vec3 p = vec3(x, y, z) + 0.5f;
                    float t = len2 > 0 ? std::clamp(glm::dot(p - a, d) / len2, 0.0f, 1.0f) : 0.0f;
                    float rad = std::max(r0 + (r1 - r0) * t, 0.5f);
                    if (glm::length(p - (a + d * t)) <= rad) r->put(ivec3(x, y, z), lo);
                }
        return 0;
    }

    int l_Set(lua_State* L) {
        Run* r = run(L);
        ivec3 p(roundi(luaL_checknumber(L, 1)), roundi(luaL_checknumber(L, 2)), roundi(luaL_checknumber(L, 3)));
        r->put(p, p);
        return 0;
    }

    int l_Get(lua_State* L) {
        Run* r = run(L);
        ivec3 p(roundi(luaL_checknumber(L, 1)), roundi(luaL_checknumber(L, 2)), roundi(luaL_checknumber(L, 3)));
        lua_pushinteger(L, r->cur ? r->handleFor(r->cur->voxels.get(p.x, p.y, p.z)) : 0);
        return 1;
    }

    // Terrain from the loaded image: red is height, green grass, blue "special" -- in the shipped
    // heightmaps it marks bare cliffs and shore, and is taken as rock. The materials are the ones
    // ground.lua creates, by handle: 1 rock, 2 dirt, 3-4 grass, 8-15 the grass map's gradient. Only a shell under the surface is filled -- the inside of a
    // hill is never seen and would cost hundreds of millions of voxels.
    int l_Heightmap(lua_State* L) {
        Run* r = run(L);
        const Image& img = r->image;
        if (img.w == 0) return 0;
        int x0 = int(luaL_checkinteger(L, 1)), y0 = int(luaL_checkinteger(L, 2));
        int x1 = int(luaL_checkinteger(L, 3)), y1 = int(luaL_checkinteger(L, 4));
        const double scale = luaL_optnumber(L, 5, 255);
        auto mat = [&](int h) { return h > 0 && size_t(h) < r->handles.size() ? r->handles[size_t(h)] : MAT_EMPTY; };
        auto height = [&](int x, int y) {
            x = std::clamp(x, 0, img.w - 1);
            y = std::clamp(y, 0, img.h - 1);
            return int(std::floor(img.at(x, y)[0] / 255.0 * scale));
        };
        LocalShape& s = r->shape();
        for (int py = std::max(0, y0); py <= std::min(y1, img.h - 1); py++)
            for (int px = std::max(0, x0); px <= std::min(x1, img.w - 1); px++) {
                const uint8_t* p = img.at(px, py);
                const int h = height(px, py);
                const int floorH = std::min({height(px - 1, py), height(px + 1, py), height(px, py - 1), height(px, py + 1), h}) - 3;
                // Green is grass (tinted by the grass map when there is one); blue marks bare
                // ground -- rock where it is strong, dirt where it is faint.
                MatId top;
                if (p[1] > 0) {
                    if (r->hasGrass && px < r->grass.w && py < r->grass.h) {
                        top = mat(8 + std::min(7, r->grass.at(px, py)[0] * 8 / 256));
                    } else {
                        top = mat(3 + int(std::uniform_int_distribution<int>(0, 1)(r->rng)));
                    }
                } else top = mat(p[2] > 127 ? 1 : 2);
                if (!top) top = mat(2);
                for (int y = std::max(0, floorH); y <= h; y++) {
                    MatId m = y == h ? top : (h - y <= 3 ? mat(2) : mat(1));
                    if (m) s.voxels.set(px - x0, y, py - y0, m);
                }
            }
        return 0;
    }

    int l_DebugPrint(lua_State* L) {
        logVerbose("voxscript: %s", luaL_tolstring(L, 1, nullptr));
        return 0;
    }

    void instructionLimit(lua_State* L, lua_Debug*) { luaL_error(L, "voxscript ran too long"); }

    const luaL_Reg API[] = {
        {"GetFloat", l_GetFloat}, {"GetInt", l_GetInt}, {"GetBool", l_GetBool}, {"GetString", l_GetString},
        {"GetColor", l_GetColor}, {"Randomize", l_Randomize}, {"GetRandomFloat", l_RandomFloat},
        {"RandomFloat", l_RandomFloat}, {"GetRandomInt", l_RandomInt}, {"RandomInt", l_RandomInt},
        {"CreateMaterial", l_CreateMaterial}, {"CreateBrush", l_CreateBrush}, {"FlipBrush", l_FlipBrush},
        {"RotateBrush", l_RotateBrush}, {"TranslateBrush", l_TranslateBrush}, {"GetBrushSize", l_GetBrushSize},
        {"Material", l_Material}, {"LoadImage", l_LoadImage}, {"GetImageSize", l_GetImageSize},
        {"GetImagePixel", l_GetImagePixel}, {"Vox", l_Vox}, {"Box", l_Box}, {"Sphere", l_Sphere}, {"Line", l_Line},
        {"Set", l_Set}, {"Get", l_Get}, {"Heightmap", l_Heightmap}, {"DebugPrint", l_DebugPrint},
        {nullptr, nullptr}};
}

bool buildVoxscript(const pugi::xml_node& n, const ShapeContext& ctx, std::vector<LocalShape>& out) {
    const std::string file = n.attribute("file").as_string();
    const std::string path = ctx.paths.resolve(file, ctx.xmlDir);
    if (path.empty()) {
        logWarn("voxscript '%s' not found", file.c_str());
        return false;
    }

    Run r;
    r.params = n.child("parameters");
    r.ctx = &ctx;
    r.scriptDir = fs::path(path).parent_path().string();
    r.out = &out;
    // Unseeded scripts would take the time in Teardown. A seed from the node's position keeps a
    // forest varied and every conversion identical.
    r.rng.seed(uint32_t(std::hash<std::string>()(std::string(n.attribute("pos").as_string()) + file)));

    lua_State* L = luaL_newstate();
    luaL_requiref(L, "_G", luaopen_base, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_settop(L, 0);
    lua_pushglobaltable(L);
    lua_pushlightuserdata(L, &r);
    luaL_setfuncs(L, API, 1);
    lua_pop(L, 1);
    // Teardown's Lua is 5.1-flavoured.
    luaL_dostring(L, "unpack = table.unpack; math.pow = function(a, b) return a ^ b end; "
                     "loadstring = load; math.mod = math.fmod");
    lua_sethook(L, instructionLimit, LUA_MASKCOUNT, 400000000);

    bool ok = luaL_dofile(L, path.c_str()) == LUA_OK;
    if (ok) {
        lua_getglobal(L, "init");
        if (lua_isfunction(L, -1)) ok = lua_pcall(L, 0, 0, 0) == LUA_OK;
        else lua_pop(L, 1);
    }
    if (!ok) logWarn("voxscript '%s': %s", file.c_str(), lua_tostring(L, -1));
    lua_close(L);
    std::erase_if(out, [](const LocalShape& s) { return s.voxels.empty(); });
    return ok || !out.empty();
}

} // namespace teardown

#else // !PROJV_TEARDOWN_LUA

#include "pugixml.hpp"
#include "shapes.h"

namespace teardown {

// Built without Lua (the scene editor's submodule was not checked out): voxscript geometry --
// trees, windows, generated walls and terrain -- is left out, and said so once.
bool buildVoxscript(const pugi::xml_node&, const ShapeContext&, std::vector<LocalShape>&) {
    static bool warned = false;
    if (!warned) {
        logWarn("this build has no Lua, so voxscripts (trees, windows, terrain) are skipped; "
                "check out examples/60-scene-editor/external/lua and rebuild");
        warned = true;
    }
    return false;
}

} // namespace teardown

#endif // PROJV_TEARDOWN_LUA
