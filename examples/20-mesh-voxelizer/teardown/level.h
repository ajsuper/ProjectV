#ifndef PROJECTV_TEARDOWN_LEVEL_H
#define PROJECTV_TEARDOWN_LEVEL_H
// A Teardown level, read from its XML into voxel canvases.
//
// The level file is a scene graph. Groups, instances (prefabs), scripts and vehicles only carry a
// transform; bodies own shapes; shapes (vox, voxbox, voxagon, voxscript, compound) own voxels, and
// a shape's child shapes are merged into it -- which is how a "hole" voxagon or a window voxscript
// cuts its parent wall. Everything that is not geometry (lights, joints, triggers, water, scripts'
// behaviour) is skipped.
//
// Static geometry -- shapes outside any body, and bodies that are not dynamic -- lands in one world
// canvas. Each dynamic body gets a canvas of its own in its own frame, so it can be placed (and later
// simulated) as a separate object.

#include <memory>
#include <string>
#include <vector>

#include "canvas.h"
#include "common.h"

namespace pugi { class xml_node; }

namespace teardown {

// Where a level's files are. Teardown paths are prefixed: MOD/ is the mod (or DLC) root, LEVEL/
// the folder named after the level file, BUILT-IN/ the game's data/built-in. Unprefixed paths are
// tried against the referencing file's folder, the mod, and the game's data folders in turn.
struct PathContext {
    std::string modRoot;   // MOD/
    std::string levelDir;  // LEVEL/
    std::string dataDir;   // <teardown>/data
    std::string resolve(const std::string& path, const std::string& fromDir) const;
};

struct LevelSource {
    std::string id;        // output folder name, e.g. "castle" or "artvandals_mansion"
    std::string title;     // human name from info.txt, when there is one
    std::string xmlPath;
    PathContext paths;
};

struct Body {
    std::string name;
    Transform frame;       // world placement of the canvas's frame
    Canvas voxels;         // TD_VOXEL voxels
};

// Water is drawn as a surface layer of translucent voxels, coarser than the level's own: a 800 m
// ocean is 4 million of these and would be 64 million at 0.1 m.
constexpr float WATER_VOXEL = 0.4f;

struct Level {
    Canvas world;          // static geometry, world frame, TD_VOXEL voxels
    Canvas water;          // water surfaces, world frame, WATER_VOXEL voxels
    std::vector<std::unique_ptr<Body>> bodies;
    vec3 spawn{0.0f};
    bool hasSpawn = false;
    // Stats for the report.
    size_t shapes = 0, prefabs = 0, voxscripts = 0, skippedScripts = 0;
    size_t transparentRemoved = 0;
};

struct LevelOptions {
    bool separateDynamic = true; // dynamic bodies as their own components; false bakes them into the world
    bool runVoxscripts = true;
    bool water = true;
    // False leaves out every voxel that is not fully opaque -- glass, mostly -- so a window is an
    // opening a renderer without transparency can see through. Water is the caller's to drop.
    bool transparent = true;
};

bool buildLevel(const LevelSource& src, const LevelOptions& options, Level& out);

// Attribute helpers shared by the shape builders.
vec3  attrVec3(const pugi::xml_node& n, const char* name, vec3 def);
float attrFloat(const pugi::xml_node& n, const char* name, float def);
bool  attrBool(const pugi::xml_node& n, const char* name, bool def);
Transform nodeTransform(const pugi::xml_node& n);

} // namespace teardown

#endif // PROJECTV_TEARDOWN_LEVEL_H
