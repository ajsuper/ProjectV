#include "shapes.h"

#include <algorithm>
#include <cstring>

#include "pugixml.hpp"
#include "vox_file.h"

namespace teardown {

namespace {
    ivec3 attrIVec3(const pugi::xml_node& n, const char* name, ivec3 def) {
        vec3 v = attrVec3(n, name, vec3(def));
        return ivec3(int(std::lround(v.x)), int(std::lround(v.y)), int(std::lround(v.z)));
    }

    glm::bvec3 mirrorOf(const pugi::xml_node& n) {
        return glm::bvec3(attrBool(n, "mirrorx", false), attrBool(n, "mirrory", false), attrBool(n, "mirrorz", false));
    }

    bool isHoleBrush(const std::string& brush) {
        return brush == "hole" || brush == "hole.vox" || brush.ends_with("/hole.vox");
    }

    // What a voxbox or voxagon is filled with: a solid material, a tiled brush, or a hole.
    struct Fill {
        enum Kind { Solid, Tiled, Hole } kind = Solid;
        MatId solid = MAT_EMPTY;
        Brush brush;

        MatId at(int x, int y, int z) const {
            if (kind == Hole) return MAT_HOLE;
            if (kind == Solid) return solid;
            return brush.sample(x, y, z);
        }
    };

    bool makeFill(const pugi::xml_node& n, const ShapeContext& ctx, Fill& fill) {
        const std::string brush = n.attribute("brush").as_string();
        if (isHoleBrush(brush)) {
            fill.kind = Fill::Hole;
            return true;
        }
        if (!brush.empty()) {
            const std::string path = ctx.paths.resolve(brush, ctx.xmlDir);
            if (path.empty() || !makeBrush(path, n.attribute("object").as_string(), fill.brush)) {
                logWarn("brush '%s' not found", brush.c_str());
                return false;
            }
            fill.kind = Fill::Tiled;
            fill.brush.offset = attrIVec3(n, "offset", ivec3(0));
            return true;
        }
        // A plain box: Teardown's material type, colour and the pbr quad (reflectivity, shininess,
        // metallic, emissive).
        const vec3 colour = attrVec3(n, "color", vec3(1.0f));
        float pbr[4] = {0, 0, 0, 0};
        std::sscanf(n.attribute("pbr").as_string(), "%f %f %f %f", &pbr[0], &pbr[1], &pbr[2], &pbr[3]);
        const Substance s = substanceFromName(n.attribute("material").as_string("none"));
        fill.solid = materials().intern(makeMaterial(s, colour.r, colour.g, colour.b, 1.0f, pbr[0], pbr[1], pbr[2], pbr[3]));
        return true;
    }
}

bool buildVox(const pugi::xml_node& n, const ShapeContext& ctx, LocalShape& out) {
    const std::string fileAttr = n.attribute("file").as_string();
    if (fileAttr.empty()) return false; // a prefab's placeholder, filled in by scripts in the game
    const std::string path = ctx.paths.resolve(fileAttr, ctx.xmlDir);
    if (path.empty()) {
        logWarn("vox file '%s' not found", fileAttr.c_str());
        return false;
    }
    auto file = loadVox(path);
    if (!file) return false;
    const std::string object = n.attribute("object").as_string();
    if (!object.empty()) {
        const VoxInstance* inst = file->find(object);
        if (!inst) {
            logWarn("vox file '%s' has no object '%s'", fileAttr.c_str(), object.c_str());
            return false;
        }
        modelToCanvas(*file, file->models[size_t(inst->model)], out.voxels, mirrorOf(n));
    } else {
        fileToCanvas(*file, out.voxels, mirrorOf(n));
    }
    out.voxelSize = TD_VOXEL * attrFloat(n, "scale", 1.0f);
    return true;
}

bool buildVoxbox(const pugi::xml_node& n, const ShapeContext& ctx, LocalShape& out) {
    const ivec3 size = glm::max(attrIVec3(n, "size", ivec3(10)), ivec3(0));
    Fill fill;
    if (!makeFill(n, ctx, fill)) return false;
    const glm::bvec3 mirror = mirrorOf(n);
    // Boxes this large are terrain slabs; their inside can never be seen, and filling it solid
    // would cost gigabytes. Keep a shell.
    const int64_t volume = int64_t(size.x) * size.y * size.z;
    const int shell = volume > (int64_t(1) << 26) ? 4 : 0;
    for (int z = 0; z < size.z; z++)
        for (int y = 0; y < size.y; y++)
            for (int x = 0; x < size.x; x++) {
                if (shell && x >= shell && y >= shell && z >= shell && x < size.x - shell && y < size.y - shell &&
                    z < size.z - shell) {
                    x = size.x - shell - 1;
                    continue;
                }
                MatId m = fill.at(x, y, z);
                if (!m) continue;
                out.voxels.set(mirror.x ? size.x - 1 - x : x, mirror.y ? size.y - 1 - y : y,
                               mirror.z ? size.z - 1 - z : z, m);
            }
    return true;
}

bool buildVoxagon(const pugi::xml_node& n, const ShapeContext& ctx, LocalShape& out) {
    std::vector<glm::vec2> poly;
    for (pugi::xml_node v : n.children("vertex")) {
        glm::vec2 p(0.0f);
        std::sscanf(v.attribute("pos").as_string(), "%f %f", &p.x, &p.y);
        poly.push_back(p / TD_VOXEL); // in voxels
    }
    if (poly.size() < 3) return false;
    const int extrude = std::max(1, n.attribute("extrude").as_int(1));
    Fill fill;
    if (!makeFill(n, ctx, fill)) return false;

    // The brush's axes follow `axis`: "y" (the default) tiles it as it is; "x" and "z" turn the
    // brush so its up axis runs along that axis of the polygon.
    const std::string axis = n.attribute("axis").as_string("y");
    glm::imat3x3 swap(1);
    if (axis == "x") swap = glm::imat3x3(0, 1, 0, 1, 0, 0, 0, 0, 1);
    else if (axis == "z") swap = glm::imat3x3(1, 0, 0, 0, 0, 1, 0, 1, 0);
    fill.brush.xform = swap;

    glm::vec2 lo(1e30f), hi(-1e30f);
    for (auto p : poly) { lo = glm::min(lo, p); hi = glm::max(hi, p); }
    const int x0 = int(std::floor(lo.x)), x1 = int(std::ceil(hi.x));
    const int z0 = int(std::floor(lo.y)), z1 = int(std::ceil(hi.y));
    std::vector<float> crossings;
    for (int z = z0; z < z1; z++) {
        // Scanline: the polygon's edge crossings at this row's centre, then fill between pairs.
        const float cz = float(z) + 0.5f;
        crossings.clear();
        for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
            const glm::vec2 a = poly[j], b = poly[i];
            if ((a.y > cz) != (b.y > cz)) crossings.push_back(a.x + (cz - a.y) / (b.y - a.y) * (b.x - a.x));
        }
        std::sort(crossings.begin(), crossings.end());
        for (size_t c = 0; c + 1 < crossings.size(); c += 2) {
            const int xa = std::max(x0, int(std::ceil(crossings[c] - 0.5f)));
            const int xb = std::min(x1 - 1, int(std::floor(crossings[c + 1] - 0.5f)));
            for (int x = xa; x <= xb; x++)
                for (int y = 0; y < extrude; y++) {
                    MatId m = fill.at(x, y, z);
                    if (m) out.voxels.set(x, y, z, m);
                }
        }
    }
    return true;
}

} // namespace teardown
