#include "vox_file.h"

#include <climits>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <zlib.h>

namespace teardown {

namespace {
    using Dict = std::map<std::string, std::string>;

    struct Reader {
        const uint8_t* p;
        const uint8_t* end;
        bool ok = true;
        // Three in file order. Never `ivec3(i32(), i32(), i32())`: argument evaluation order is
        // unspecified, and GCC reads them backwards.
        ivec3 ivec() {
            int32_t x = i32();
            int32_t y = i32();
            int32_t z = i32();
            return ivec3(x, y, z);
        }
        int32_t i32() {
            if (end - p < 4) { ok = false; p = end; return 0; }
            int32_t v;
            std::memcpy(&v, p, 4);
            p += 4;
            return v;
        }
        std::string str() {
            int32_t n = i32();
            if (n < 0 || end - p < n) { ok = false; p = end; return {}; }
            std::string s(reinterpret_cast<const char*>(p), size_t(n));
            p += n;
            return s;
        }
        Dict dict() {
            Dict d;
            int32_t n = i32();
            for (int32_t i = 0; i < n && ok; i++) {
                std::string k = str();
                d[k] = str();
            }
            return d;
        }
    };

    float dictFloat(const Dict& d, const char* k, float def) {
        auto it = d.find(k);
        if (it == d.end()) return def;
        try { return std::stof(it->second); } catch (...) { return def; }
    }

    glm::imat3x3 decodeRotation(int r) {
        glm::imat3x3 m(0);
        int i0 = r & 3, i1 = (r >> 2) & 3;
        if (i0 > 2 || i1 > 2 || i0 == i1) return glm::imat3x3(1);
        int i2 = 3 - i0 - i1;
        m[i0][0] = (r >> 4) & 1 ? -1 : 1;
        m[i1][1] = (r >> 5) & 1 ? -1 : 1;
        m[i2][2] = (r >> 6) & 1 ? -1 : 1;
        return m;
    }

    struct Node {
        enum Kind { Transform, Group, Shape } kind = Group;
        Dict attrs;
        int child = -1;                // transform
        Dict frame;                    // transform, frame 0
        std::vector<int> children;     // group
        std::vector<int> models;       // shape
    };

    // The default palette's look is irrelevant to Teardown content (every level vox carries RGBA);
    // a missing one only happens with hand-made test files.
    uint32_t defaultColour(int i) { return 0xFF000000u | uint32_t(i) * 0x010101u; }

    VoxMaterial paletteMaterial(int index, uint32_t rgba, const Dict* matl) {
        VoxMaterial m;
        m.r = uint8_t(rgba & 0xFF);
        m.g = uint8_t((rgba >> 8) & 0xFF);
        m.b = uint8_t((rgba >> 16) & 0xFF);
        m.substance = substanceFromPaletteIndex(index);
        float alpha = m.substance == Substance::Glass ? 0.5f : 1.0f;
        float gloss = 0.0f, metal = 0.0f, emissive = 0.0f;
        if (matl) {
            auto t = matl->find("_type");
            std::string type = t == matl->end() ? "_diffuse" : t->second;
            float rough = dictFloat(*matl, "_rough", 1.0f);
            if (type == "_metal" || type == "_plastic" || type == "_blend") {
                gloss = 0.5f * (1.0f - rough);
                metal = dictFloat(*matl, "_metal", 0.0f);
            } else if (type == "_glass") {
                alpha = std::min(alpha, 0.5f);
                gloss = 0.5f;
            } else if (type == "_emit") {
                emissive = dictFloat(*matl, "_emit", 0.0f) * (1.0f + dictFloat(*matl, "_flux", 0.0f)) * 2.0f;
            }
        }
        m.alpha = uint8_t(std::lround(std::clamp(alpha, 0.0f, 1.0f) * 255.0f));
        m.gloss = uint8_t(std::lround(std::clamp(gloss, 0.0f, 1.0f) * 255.0f));
        m.metal = uint8_t(std::lround(std::clamp(metal, 0.0f, 1.0f) * 255.0f));
        m.emissive = uint8_t(std::clamp(std::lround(emissive * 16.0f), 0L, 255L));
        return m;
    }

    std::shared_ptr<VoxFile> parse(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return nullptr;
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (bytes.size() < 8 || std::memcmp(bytes.data(), "VOX ", 4) != 0) return nullptr;

        auto file = std::make_shared<VoxFile>();
        file->path = path;
        std::array<uint32_t, 256> rgba;
        for (int i = 0; i < 256; i++) rgba[i] = defaultColour(i);
        std::map<int, Dict> matl;
        std::map<int, Node> nodes;
        ivec3 pendingSize(0);

        Reader r{bytes.data() + 8, bytes.data() + bytes.size()};
        while (r.ok && r.end - r.p >= 12) {
            char id[5] = {};
            std::memcpy(id, r.p, 4);
            r.p += 4;
            int32_t contentSize = r.i32();
            int32_t childrenSize = r.i32();
            if (contentSize < 0 || r.end - r.p < contentSize) break;
            Reader c{r.p, r.p + contentSize};
            const std::string cid(id);
            if (cid == "MAIN") {
                // Its children follow inline; just keep reading.
                continue;
            } else if (cid == "SIZE") {
                pendingSize = c.ivec();
            } else if (cid == "XYZI") {
                VoxModel model;
                model.size = pendingSize;
                int32_t n = c.i32();
                if (n > 0 && c.end - c.p >= int64_t(n) * 4) {
                    model.voxels.reserve(size_t(n));
                    for (int32_t i = 0; i < n; i++, c.p += 4)
                        model.voxels.push_back(VoxModel::pack(c.p[0], c.p[1], c.p[2], c.p[3]));
                }
                file->models.push_back(std::move(model));
            } else if (cid == "TDCZ") {
                // Teardown's own voxel chunk: the size again, then a zlib stream of one palette index
                // per voxel, x fastest. The stream is written without its trailing checksum, so it is
                // inflated until the volume is full rather than until zlib calls it complete.
                VoxModel model;
                model.size = c.ivec();
                const size_t volume = size_t(std::max(0, model.size.x)) * size_t(std::max(0, model.size.y)) * size_t(std::max(0, model.size.z));
                std::vector<uint8_t> dense(volume);
                z_stream zs{};
                if (volume && inflateInit(&zs) == Z_OK) {
                    zs.next_in = const_cast<Bytef*>(c.p);
                    zs.avail_in = uInt(c.end - c.p);
                    zs.next_out = dense.data();
                    zs.avail_out = uInt(volume);
                    int rc = Z_OK;
                    while (zs.avail_out > 0 && rc == Z_OK) rc = inflate(&zs, Z_NO_FLUSH);
                    inflateEnd(&zs);
                    if (zs.avail_out > 0) logWarn("%s: a TDCZ model is truncated", path.c_str());
                    size_t i = 0;
                    for (int z = 0; z < model.size.z; z++)
                        for (int y = 0; y < model.size.y; y++)
                            for (int x = 0; x < model.size.x; x++, i++)
                                if (dense[i]) model.voxels.push_back(VoxModel::pack(x, y, z, dense[i]));
                }
                file->models.push_back(std::move(model));
            } else if (cid == "RGBA") {
                for (int i = 0; i < 256 && c.end - c.p >= 4; i++) {
                    std::memcpy(&rgba[(i + 1) & 0xFF], c.p, 4);
                    c.p += 4;
                }
            } else if (cid == "MATL") {
                int32_t mid = c.i32();
                matl[mid & 0xFF] = c.dict();
            } else if (cid == "nTRN") {
                int32_t nid = c.i32();
                Node n;
                n.kind = Node::Transform;
                n.attrs = c.dict();
                n.child = c.i32();
                c.i32(); // reserved
                c.i32(); // layer
                int32_t frames = c.i32();
                if (frames > 0) n.frame = c.dict();
                nodes[nid] = std::move(n);
            } else if (cid == "nGRP") {
                int32_t nid = c.i32();
                Node n;
                n.kind = Node::Group;
                n.attrs = c.dict();
                int32_t count = c.i32();
                for (int32_t i = 0; i < count && c.ok; i++) n.children.push_back(c.i32());
                nodes[nid] = std::move(n);
            } else if (cid == "nSHP") {
                int32_t nid = c.i32();
                Node n;
                n.kind = Node::Shape;
                n.attrs = c.dict();
                int32_t count = c.i32();
                for (int32_t i = 0; i < count && c.ok; i++) {
                    n.models.push_back(c.i32());
                    c.dict();
                }
                nodes[nid] = std::move(n);
            }
            r.p += contentSize;
            (void)childrenSize;
        }

        for (int i = 1; i < 256; i++) {
            auto it = matl.find(i);
            file->palette[i] = materials().intern(paletteMaterial(i, rgba[i], it == matl.end() ? nullptr : &it->second));
        }
        // Teardown reserves the last palette entry for "hole": it carves the shape it is merged into.
        file->palette[255] = MAT_HOLE;

        // Flatten the scene graph. Files without one hold their models at the origin, unnamed.
        if (nodes.empty()) {
            for (size_t i = 0; i < file->models.size(); i++) {
                VoxInstance inst;
                inst.model = int(i);
                const ivec3 s = file->models[i].size;
                inst.translation = ivec3(0, 0, s.z / 2);
                file->instances.push_back(inst);
            }
            return file;
        }
        struct Walk {
            const std::map<int, Node>& nodes;
            VoxFile& file;
            int depth = 0;
            void go(int id, const glm::imat3x3& rot, ivec3 t, const std::string& name) {
                auto it = nodes.find(id);
                if (it == nodes.end() || ++depth > 64) { depth--; return; }
                const Node& n = it->second;
                if (n.kind == Node::Transform) {
                    glm::imat3x3 r(1);
                    ivec3 lt(0);
                    if (auto f = n.frame.find("_r"); f != n.frame.end()) r = decodeRotation(std::atoi(f->second.c_str()));
                    if (auto f = n.frame.find("_t"); f != n.frame.end())
                        std::sscanf(f->second.c_str(), "%d %d %d", &lt.x, &lt.y, &lt.z);
                    std::string nm = name;
                    if (auto a = n.attrs.find("_name"); a != n.attrs.end()) nm = a->second;
                    go(n.child, rot * r, t + rot * lt, nm);
                } else if (n.kind == Node::Group) {
                    for (int c : n.children) go(c, rot, t, name);
                } else {
                    for (int m : n.models) {
                        if (m < 0 || size_t(m) >= file.models.size()) continue;
                        file.instances.push_back(VoxInstance{name, m, rot, t});
                    }
                }
                depth--;
            }
        } walk{nodes, *file};
        walk.go(0, glm::imat3x3(1), ivec3(0), "");
        return file;
    }

    // Writes one MagicaVoxel-space voxel index into a canvas in Teardown's frame.
    inline void put(Canvas& out, ivec3 mv, MatId m, glm::bvec3 mirror) {
        ivec3 t(mv.x, mv.z, -mv.y - 1);
        if (mirror.x) t.x = -t.x - 1;
        if (mirror.y) t.y = -t.y - 1;
        if (mirror.z) t.z = -t.z - 1;
        out.set(t.x, t.y, t.z, m);
    }
}

uint8_t VoxModel::at(int x, int y, int z) const {
    if (x < 0 || y < 0 || z < 0 || x >= size.x || y >= size.y || z >= size.z) return 0;
    if (dense.empty()) {
        dense.assign(size_t(size.x) * size.y * size.z, 0);
        for (uint64_t v : voxels) {
            const ivec3 p = posOf(v);
            if (p.x < size.x && p.y < size.y && p.z < size.z) dense[p.x + size_t(size.x) * (p.y + size_t(size.y) * p.z)] = indexOf(v);
        }
    }
    return dense[x + size_t(size.x) * (y + size_t(size.y) * z)];
}

const VoxInstance* VoxFile::find(const std::string& object) const {
    if (instances.empty()) return nullptr;
    if (object.empty()) return &instances.front();
    for (const auto& inst : instances)
        if (inst.name == object) return &inst;
    return nullptr;
}

std::shared_ptr<const VoxFile> loadVox(const std::string& path) {
    static std::mutex mutex;
    static std::unordered_map<std::string, std::shared_ptr<const VoxFile>> cache;
    std::lock_guard<std::mutex> lock(mutex);
    if (auto it = cache.find(path); it != cache.end()) return it->second;
    auto file = parse(path);
    if (!file) logWarn("cannot read vox file '%s'", path.c_str());
    cache[path] = file;
    return file;
}

void modelToCanvas(const VoxFile& file, const VoxModel& model, Canvas& out, glm::bvec3 mirror) {
    const ivec3 half(model.size.x / 2, model.size.y / 2, 0);
    for (uint64_t v : model.voxels) {
        const uint8_t index = VoxModel::indexOf(v);
        if (!index) continue;
        put(out, VoxModel::posOf(v) - half, file.palette[index], mirror);
    }
}

void fileToCanvas(const VoxFile& file, Canvas& out, glm::bvec3 mirror) {
    // Every instance where the file's scene graph puts it, about MagicaVoxel's own origin. A model
    // MagicaVoxel placed by default sits centred on that origin with its base on z = 0, which is
    // exactly the pivot a named object gets, so single-model files land the same either way.
    for (const VoxInstance& inst : file.instances) {
        const VoxModel& model = file.models[size_t(inst.model)];
        const glm::dmat3 R(inst.rot);
        const dvec3 centre = dvec3(model.size) * 0.5;
        for (uint64_t v : model.voxels) {
            const uint8_t index = VoxModel::indexOf(v);
            if (!index) continue;
            dvec3 c = dvec3(VoxModel::posOf(v)) + 0.5 - centre;
            put(out, ivec3(glm::floor(dvec3(inst.translation) + R * c)), file.palette[index], mirror);
        }
    }
}

MatId Brush::sample(int x, int y, int z) const {
    if (!model || size.x <= 0 || size.y <= 0 || size.z <= 0) return MAT_EMPTY;
    ivec3 p = xform * ivec3(x, y, z) + offset;
    p = ivec3(floorMod(p.x, size.x), floorMod(p.y, size.y), floorMod(p.z, size.z));
    // Teardown (x, y, z) -> MagicaVoxel (x, size_y - 1 - z, y).
    uint8_t index = model->at(p.x, model->size.y - 1 - p.z, p.y);
    return index ? file->palette[index] : MAT_EMPTY;
}

bool makeBrush(const std::string& path, const std::string& object, Brush& out) {
    auto file = loadVox(path);
    if (!file) return false;
    const VoxInstance* inst = file->find(object);
    if (!inst) {
        if (!object.empty()) logWarn("vox file '%s' has no object '%s'", path.c_str(), object.c_str());
        if (file->models.empty()) return false;
    }
    out.file = file;
    out.model = &file->models[inst ? size_t(inst->model) : 0];
    out.size = ivec3(out.model->size.x, out.model->size.z, out.model->size.y);
    return true;
}

} // namespace teardown
