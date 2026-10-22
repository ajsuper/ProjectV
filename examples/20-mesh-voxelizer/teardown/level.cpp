#include "level.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <sstream>

#include "pugixml.hpp"
#include "shapes.h"

namespace fs = std::filesystem;

namespace teardown {

// ---- Attributes -----------------------------------------------------------------------------

vec3 attrVec3(const pugi::xml_node& n, const char* name, vec3 def) {
    pugi::xml_attribute a = n.attribute(name);
    if (!a) return def;
    vec3 v = def;
    std::sscanf(a.as_string(), "%f %f %f", &v.x, &v.y, &v.z);
    return v;
}

float attrFloat(const pugi::xml_node& n, const char* name, float def) {
    pugi::xml_attribute a = n.attribute(name);
    return a ? a.as_float(def) : def;
}

bool attrBool(const pugi::xml_node& n, const char* name, bool def) {
    pugi::xml_attribute a = n.attribute(name);
    if (!a) return def;
    std::string s = a.as_string();
    return s == "true" || s == "1";
}

Transform nodeTransform(const pugi::xml_node& n) {
    Transform t;
    t.pos = attrVec3(n, "pos", vec3(0.0f));
    const vec3 r = attrVec3(n, "rot", vec3(0.0f));
    t.rot = quatEuler(r.x, r.y, r.z);
    return t;
}

// ---- Paths ----------------------------------------------------------------------------------

namespace {
    bool startsWithNoCase(const std::string& s, const char* prefix) {
        size_t n = std::strlen(prefix);
        if (s.size() < n) return false;
        for (size_t i = 0; i < n; i++)
            if (std::tolower(uint8_t(s[i])) != std::tolower(uint8_t(prefix[i]))) return false;
        return true;
    }

    std::string lower(std::string s) {
        for (char& c : s) c = char(std::tolower(uint8_t(c)));
        return s;
    }

    // The game ships for Windows, where paths are case-insensitive, and level files are not
    // consistent about case. Exact first; then each component matched without case.
    std::string findFile(const fs::path& base, const std::string& rel) {
        std::error_code ec;
        fs::path direct = base / rel;
        if (fs::is_regular_file(direct, ec)) return direct.lexically_normal().string();
        static std::map<std::string, std::map<std::string, std::string>> listings;
        fs::path cur = base;
        std::stringstream ss(rel);
        std::string part;
        while (std::getline(ss, part, '/')) {
            if (part.empty() || part == ".") continue;
            if (part == "..") { cur = cur.parent_path(); continue; }
            auto& listing = listings[cur.string()];
            if (listing.empty() && fs::is_directory(cur, ec)) {
                for (const auto& e : fs::directory_iterator(cur, ec)) listing[lower(e.path().filename().string())] = e.path().filename().string();
                listing[""] = "";
            }
            auto it = listing.find(lower(part));
            if (it == listing.end()) return {};
            cur /= it->second;
        }
        return fs::is_regular_file(cur, ec) ? cur.lexically_normal().string() : std::string();
    }
}

std::string PathContext::resolve(const std::string& raw, const std::string& fromDir) const {
    if (raw.empty()) return {};
    std::string p = raw;
    std::replace(p.begin(), p.end(), '\\', '/');
    if (startsWithNoCase(p, "MOD/")) return findFile(modRoot, p.substr(4));
    if (startsWithNoCase(p, "LEVEL/")) return findFile(levelDir, p.substr(6));
    if (startsWithNoCase(p, "BUILT-IN/")) return findFile(fs::path(dataDir) / "built-in", p.substr(9));
    const fs::path data(dataDir);
    for (const fs::path& base : {fs::path(fromDir), fs::path(modRoot), data / "built-in" / "vox", data / "built-in",
                                 data / "vox", data, fs::path(levelDir)}) {
        if (base.empty()) continue;
        std::string f = findFile(base, p);
        if (!f.empty()) return f;
    }
    return {};
}

// ---- The walk -------------------------------------------------------------------------------

namespace {
    const std::set<std::string> SHAPE_TAGS = {"vox", "voxbox", "voxagon", "voxscript", "compound"};
    // Nodes that only carry a transform for what is under them.
    const std::set<std::string> GROUP_TAGS = {"scene", "prefab", "group", "script", "rig", "animator", "vehicle",
                                              "location", "screen", "trigger", "light", "joint", "spawnpoint"};

    struct Walker {
        const LevelSource& src;
        const LevelOptions& options;
        Level& level;
        std::set<std::string> activeLayers{"sandbox"};
        std::map<std::string, std::unique_ptr<pugi::xml_document>> prefabs;
        int depth = 0;
        std::string currentFile; // for --td-verbosity 3: which file the node being walked came from
        struct Water { pugi::xml_node node; Transform world; };
        std::vector<Water> waters;
        std::vector<glm::vec2> boundary; // world x, z

        // Each water node as a surface layer one voxel deep, its top at the node's height. A water
        // is a rectangle (`size`), a polygon (vertices), or neither -- the whole level, held to the
        // boundary when there is one and to the geometry's extent when not.
        void buildWater() {
            if (waters.empty()) return;
            ivec3 lo, hi;
            const bool any = level.world.bounds(lo, hi);
            for (const Water& w : waters) {
                std::vector<glm::vec2> poly; // in the node's local x, z, metres
                for (pugi::xml_node v : w.node.children("vertex")) {
                    glm::vec2 p(0.0f);
                    std::sscanf(v.attribute("pos").as_string(), "%f %f", &p.x, &p.y);
                    poly.push_back(p);
                }
                glm::vec2 size(0.0f);
                std::sscanf(w.node.attribute("size").as_string(""), "%f %f", &size.x, &size.y);
                if (poly.size() < 3 && size.x > 0 && size.y > 0) {
                    poly = {{-size.x / 2, -size.y / 2}, {size.x / 2, -size.y / 2}, {size.x / 2, size.y / 2}, {-size.x / 2, size.y / 2}};
                }
                if (poly.size() < 3) {
                    // Unbounded: in the game it runs to the horizon. Here it covers the level's
                    // footprint and the boundary, and 20 m around both.
                    glm::vec2 a(1e30f), b(-1e30f);
                    for (auto p : boundary) { a = glm::min(a, p); b = glm::max(b, p); }
                    if (any) {
                        a = glm::min(a, glm::vec2(lo.x, lo.z) * TD_VOXEL);
                        b = glm::max(b, glm::vec2(hi.x, hi.z) * TD_VOXEL);
                    }
                    if (a.x > b.x) continue;
                    a -= 20.0f;
                    b += 20.0f;
                    const Transform inv = w.world.inverse();
                    for (auto p : {glm::vec2(a.x, a.y), glm::vec2(b.x, a.y), glm::vec2(b.x, b.y), glm::vec2(a.x, b.y)}) {
                        vec3 l = inv.apply(vec3(p.x, w.world.pos.y, p.y));
                        poly.emplace_back(l.x, l.z);
                    }
                }
                const vec3 c = attrVec3(w.node, "color", vec3(0.05f, 0.2f, 0.3f));
                const vec3 tint = glm::clamp(c * 3.0f + vec3(0.04f, 0.10f, 0.14f), 0.0f, 1.0f);
                VoxMaterial m = makeMaterial(Substance::None, tint.r, tint.g, tint.b, 0.45f, 0.9f);
                const MatId id = materials().intern(m);

                // Rasterise the polygon in the node's frame at WATER_VOXEL, then place it.
                Canvas local;
                glm::vec2 a(1e30f), b(-1e30f);
                for (auto& p : poly) { a = glm::min(a, p); b = glm::max(b, p); }
                const int x0 = int(std::floor(a.x / WATER_VOXEL)), x1 = int(std::ceil(b.x / WATER_VOXEL));
                const int z0 = int(std::floor(a.y / WATER_VOXEL)), z1 = int(std::ceil(b.y / WATER_VOXEL));
                for (int z = z0; z < z1; z++)
                    for (int x = x0; x < x1; x++) {
                        const glm::vec2 q((x + 0.5f) * WATER_VOXEL, (z + 0.5f) * WATER_VOXEL);
                        bool inside = false;
                        for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
                            if ((poly[i].y > q.y) != (poly[j].y > q.y) &&
                                q.x < (poly[j].x - poly[i].x) * (q.y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x)
                                inside = !inside;
                        if (inside) local.set(x, -1, z, id);
                    }
                stamp(level.water, WATER_VOXEL, local, WATER_VOXEL, w.world, Holes::Skip);
            }
        }

        // Layer filter: "a b" needs one of a, b active (or "all"); "-a" is dropped when a is active.
        bool layerVisible(const pugi::xml_node& n) const {
            pugi::xml_attribute a = n.attribute("layer");
            if (!a) return true;
            std::stringstream ss(a.as_string());
            std::string tok;
            bool anyPositive = false, positiveHit = false;
            while (ss >> tok) {
                if (tok == "all") return true;
                if (tok[0] == '-') {
                    if (activeLayers.count(tok.substr(1))) return false;
                } else {
                    anyPositive = true;
                    if (activeLayers.count(tok)) positiveHit = true;
                }
            }
            return !anyPositive || positiveHit;
        }

        // What an <instance> places. A prefab saved by the editor wraps everything in one group named
        // "instance=<its file>", carrying where the prefab sat when it was authored; the instance's
        // own transform replaces that, so the wrapper's is dropped and its children placed directly.
        static pugi::xml_node prefabContent(const pugi::xml_document* doc) {
            pugi::xml_node root = doc->document_element();
            pugi::xml_node first = root.find_child([](pugi::xml_node c) { return c.type() == pugi::node_element; });
            if (first && std::string(first.attribute("name").as_string()).starts_with("instance=")) return first;
            return root;
        }

        const pugi::xml_document* loadPrefab(const std::string& path) {
            auto& doc = prefabs[path];
            if (!doc) {
                doc = std::make_unique<pugi::xml_document>();
                if (!doc->load_file(path.c_str())) {
                    logWarn("cannot parse prefab '%s'", path.c_str());
                    doc->reset();
                }
                level.prefabs++;
            }
            return doc.get();
        }

        // A shape node and every shape below it (through groups), merged into one canvas in the
        // node's frame. Bodies found below it are not merged: they are handed to `walk` instead.
        bool buildShapeTree(const pugi::xml_node& n, const std::string& xmlDir, const Transform& world, LocalShape& out) {
            const std::string tag = n.name();
            ShapeContext ctx{src.paths, xmlDir};
            std::vector<LocalShape> parts;
            bool ok = true;
            if (tag == "vox") { parts.emplace_back(); ok = buildVox(n, ctx, parts.back()); }
            else if (tag == "voxbox") { parts.emplace_back(); ok = buildVoxbox(n, ctx, parts.back()); }
            else if (tag == "voxagon") { parts.emplace_back(); ok = buildVoxagon(n, ctx, parts.back()); }
            else if (tag == "voxscript") {
                if (options.runVoxscripts) { ok = buildVoxscript(n, ctx, parts); level.voxscripts++; }
                else { level.skippedScripts++; }
            }
            if (!ok) parts.clear();
            level.shapes++;

            out.local = Transform{};
            out.voxelSize = parts.size() == 1 ? parts[0].voxelSize : TD_VOXEL;
            if (parts.size() == 1 && parts[0].local.pos == vec3(0.0f) && parts[0].local.rot == quat(1, 0, 0, 0)) {
                out.voxels = std::move(parts[0].voxels);
            } else {
                for (const LocalShape& p : parts) stamp(out.voxels, out.voxelSize, p.voxels, p.voxelSize, p.local, Holes::Keep);
            }
            mergeChildren(n, xmlDir, world, Transform{}, out);
            return true;
        }

        void mergeChildren(const pugi::xml_node& parent, const std::string& xmlDir, const Transform& parentWorld,
                           const Transform& fromShape, LocalShape& out) {
            for (pugi::xml_node c : parent.children()) {
                if (c.type() != pugi::node_element || !layerVisible(c)) continue;
                const std::string tag = c.name();
                const Transform local = fromShape * nodeTransform(c);
                if (SHAPE_TAGS.count(tag)) {
                    LocalShape child;
                    buildShapeTree(c, xmlDir, parentWorld * nodeTransform(c), child);
                    stamp(out.voxels, out.voxelSize, child.voxels, child.voxelSize, local * child.local, Holes::Carve);
                } else if (tag == "body" || tag == "wheel") {
                    walkNode(c, xmlDir, parentWorld * nodeTransform(c) , nullptr);
                } else if (tag == "instance") {
                    const std::string path = src.paths.resolve(c.attribute("file").as_string(), xmlDir);
                    const pugi::xml_document* doc = path.empty() ? nullptr : loadPrefab(path);
                    if (!doc || ++depth > 32) { depth -= doc ? 1 : 0; continue; }
                    mergeChildren(prefabContent(doc), fs::path(path).parent_path().string(), parentWorld * nodeTransform(c), local,
                                  out);
                    depth--;
                } else if (GROUP_TAGS.count(tag)) {
                    mergeChildren(c, xmlDir, parentWorld * nodeTransform(c), local, out);
                }
            }
        }

        // `target` null: the static world. A shape's world transform is `world`.
        void place(const LocalShape& shape, const Transform& world, Body* target) {
            if (target) stamp(target->voxels, TD_VOXEL, shape.voxels, shape.voxelSize, target->frame.inverse() * world * shape.local, Holes::Skip);
            else stamp(level.world, TD_VOXEL, shape.voxels, shape.voxelSize, world * shape.local, Holes::Skip);
        }

        void walkNode(const pugi::xml_node& n, const std::string& xmlDir, const Transform& world, Body* target) {
            const std::string tag = n.name();
            if (SHAPE_TAGS.count(tag)) {
                LocalShape shape;
                buildShapeTree(n, xmlDir, world, shape);
                place(shape, world, target);
                if (g_verbosity > 2) {
                    ivec3 lo, hi;
                    if (shape.voxels.bounds(lo, hi)) {
                        vec3 a(1e30f), b(-1e30f);
                        for (int k = 0; k < 8; k++) {
                            const vec3 c = world.apply(vec3(k & 1 ? hi.x : lo.x, k & 2 ? hi.y : lo.y, k & 4 ? hi.z : lo.z) * shape.voxelSize);
                            a = glm::min(a, c);
                            b = glm::max(b, c);
                        }
                        std::printf("shape %s in=%s file=%s object=%s line=%td size=(%d %d %d) world=(%.1f %.1f %.1f)-(%.1f %.1f %.1f)\n", tag.c_str(),
                                    fs::path(currentFile.empty() ? src.xmlPath : currentFile).filename().c_str(),
                                    n.attribute("file").as_string(n.attribute("brush").as_string()), n.attribute("object").as_string(),
                                    n.offset_debug(), hi.x - lo.x, hi.y - lo.y, hi.z - lo.z, a.x, a.y, a.z, b.x, b.y, b.z);
                    }
                }
                return;
            }
            if (tag == "body" || tag == "wheel") {
                const bool dynamic = tag == "wheel" || attrBool(n, "dynamic", false);
                if (dynamic && options.separateDynamic) {
                    auto body = std::make_unique<Body>();
                    body->name = n.attribute("name").as_string(tag == "wheel" ? "wheel" : "body");
                    body->frame = world;
                    Body* b = body.get();
                    level.bodies.push_back(std::move(body));
                    walkChildren(n, xmlDir, world, b);
                } else {
                    walkChildren(n, xmlDir, world, target);
                }
                return;
            }
            if (tag == "instance") {
                const std::string file = n.attribute("file").as_string();
                const std::string path = src.paths.resolve(file, xmlDir);
                if (path.empty()) { logWarn("prefab '%s' not found", file.c_str()); return; }
                const pugi::xml_document* doc = loadPrefab(path);
                if (!doc || !doc->document_element() || depth > 32) return;
                depth++;
                const std::string outer = currentFile;
                currentFile = path;
                walkChildren(prefabContent(doc), fs::path(path).parent_path().string(), world, target);
                currentFile = outer;
                depth--;
                return;
            }
            if (tag == "water") {
                waters.push_back({n, world});
                return;
            }
            if (tag == "boundary") {
                for (pugi::xml_node v : n.children("vertex")) {
                    glm::vec2 p(0.0f);
                    std::sscanf(v.attribute("pos").as_string(), "%f %f", &p.x, &p.y);
                    const vec3 w = world.apply(vec3(p.x, 0.0f, p.y));
                    boundary.emplace_back(w.x, w.z);
                }
                return;
            }
            if (tag == "spawnpoint" && !level.hasSpawn) {
                level.spawn = world.pos;
                level.hasSpawn = true;
            }
            if (GROUP_TAGS.count(tag)) walkChildren(n, xmlDir, world, target);
        }

        void walkChildren(const pugi::xml_node& n, const std::string& xmlDir, const Transform& world, Body* target) {
            for (pugi::xml_node c : n.children()) {
                if (c.type() != pugi::node_element || !layerVisible(c)) continue;
                walkNode(c, xmlDir, world * nodeTransform(c), target);
            }
        }
    };
}

namespace {
    size_t dropTransparent(Canvas& canvas) {
        std::vector<ivec3> clear;
        canvas.forEach([&](int x, int y, int z, MatId m) {
            if (m != MAT_HOLE && materials().get(m).alpha < 255) clear.emplace_back(x, y, z);
        });
        for (const ivec3& p : clear) canvas.set(p.x, p.y, p.z, MAT_EMPTY);
        return clear.size();
    }
}

bool buildLevel(const LevelSource& src, const LevelOptions& options, Level& out) {
    pugi::xml_document doc;
    pugi::xml_parse_result r = doc.load_file(src.xmlPath.c_str());
    if (!r) {
        logWarn("cannot parse '%s': %s", src.xmlPath.c_str(), r.description());
        return false;
    }
    Walker w{src, options, out};
    w.walkChildren(doc.document_element(), fs::path(src.xmlPath).parent_path().string(), Transform{}, nullptr);
    if (options.water) w.buildWater();
    if (!options.transparent) {
        out.transparentRemoved += dropTransparent(out.world);
        for (auto& b : out.bodies) out.transparentRemoved += dropTransparent(b->voxels);
    }
    // Bodies that came out empty (a joint holder, a body whose shapes all failed) are dropped.
    std::erase_if(out.bodies, [](const std::unique_ptr<Body>& b) { return b->voxels.empty(); });
    return true;
}

} // namespace teardown
