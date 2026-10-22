#ifndef PROJECTV_TEARDOWN_VOX_FILE_H
#define PROJECTV_TEARDOWN_VOX_FILE_H
// MagicaVoxel .vox files, read into Teardown's frame.
//
// What Teardown takes from a .vox: each model's voxels (palette indices), the palette, the MATL
// chunk's material parameters, and the scene graph's names -- `object="name"` in a level file picks
// the shape whose transform node carries that `_name`. With no object, every shape in the file is
// loaded where the file's scene graph puts it.
//
// Frame. A Teardown vox shape's pivot is at the bottom centre of the model: MagicaVoxel x maps to
// Teardown x, MagicaVoxel z (up) to Teardown y, and MagicaVoxel y to Teardown -z, with the model
// shifted by floor(size / 2) in x and y. `VoxShape::toCanvas` writes a model in exactly that frame,
// so a <vox> node's pos/rot place it with no further offset.

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "canvas.h"
#include "common.h"
#include "materials.h"

namespace teardown {

struct VoxModel {
    ivec3 size{0};                       // MagicaVoxel axes
    // Packed x | y << 16 | z << 32 | index << 48: Teardown's own TDCZ models are not held to
    // MagicaVoxel's 256 per axis.
    std::vector<uint64_t> voxels;
    static uint64_t pack(int x, int y, int z, uint8_t i) {
        return uint64_t(uint16_t(x)) | uint64_t(uint16_t(y)) << 16 | uint64_t(uint16_t(z)) << 32 | uint64_t(i) << 48;
    }
    static ivec3 posOf(uint64_t v) { return ivec3(int(v & 0xFFFF), int((v >> 16) & 0xFFFF), int((v >> 32) & 0xFFFF)); }
    static uint8_t indexOf(uint64_t v) { return uint8_t(v >> 48); }
    // Dense lookup, built on first use (brushes are sampled per voxel).
    mutable std::vector<uint8_t> dense;
    uint8_t at(int x, int y, int z) const; // MagicaVoxel axes, 0 outside
};

// One placement of a model in the file's scene graph.
struct VoxInstance {
    std::string name;   // the nearest transform node's _name
    int model = -1;
    glm::imat3x3 rot{1}; // MagicaVoxel axes
    ivec3 translation{0};
};

struct VoxFile {
    std::string path;
    std::vector<VoxModel> models;
    std::vector<VoxInstance> instances;
    std::array<MatId, 256> palette{}; // palette index -> interned material (0 unused)

    // The instance a level file's `object` names; null when absent. Empty name = first instance.
    const VoxInstance* find(const std::string& object) const;
};

// Loads (and caches) a .vox. Null on failure, with a warning.
std::shared_ptr<const VoxFile> loadVox(const std::string& path);

// Writes a model into `out` in Teardown's shape frame (see top of file). `mirror` flips the shape in
// its own frame per axis (the level file's mirrorx / mirrory / mirrorz).
void modelToCanvas(const VoxFile& file, const VoxModel& model, Canvas& out, glm::bvec3 mirror = glm::bvec3(false));

// Writes every instance of the file where its scene graph places it, about MagicaVoxel's origin,
// in Teardown's frame.
void fileToCanvas(const VoxFile& file, Canvas& out, glm::bvec3 mirror = glm::bvec3(false));

// A brush: one model of a file, sampled with wrap-around in Teardown axes. Box sizes and offsets
// in level files are Teardown axes too, so a voxbox can tile it directly.
struct Brush {
    std::shared_ptr<const VoxFile> file;
    const VoxModel* model = nullptr;
    ivec3 size{0};      // Teardown axes: (mx, mz, my)
    ivec3 offset{0};    // added to the sample coordinate before wrapping
    // Teardown axes -> palette material; MAT_EMPTY where the brush is empty.
    MatId sample(int x, int y, int z) const;
    // A brush's voxels are transformed by Rotate/Flip in voxscripts; this maps a sample coordinate.
    glm::imat3x3 xform{1};
};

bool makeBrush(const std::string& path, const std::string& object, Brush& out);

} // namespace teardown

#endif // PROJECTV_TEARDOWN_VOX_FILE_H
