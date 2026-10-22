#ifndef PROJECTV_TEARDOWN_IMPORT_H
#define PROJECTV_TEARDOWN_IMPORT_H
// The voxelizer's Teardown input: levels read from the game's XML level files and the MagicaVoxel
// files they place, written as ProjectV compose folders. main.cpp parses the command line into
// Options and calls run(); everything Teardown-specific lives in this folder.
//
// Unlike a mesh, a Teardown level is already voxels (0.1 m each), so there is no resolution to
// choose: shapes are stamped into one world grid, dynamic bodies into components of their own.

#include <string>
#include <vector>

#include "common.h"

namespace teardown {

struct Options {
    std::string game;                  // the game folder; empty = look in the Steam libraries
    std::string levelFile;             // a level .xml given directly with -f
    std::vector<std::string> levels;   // ids of installed levels to convert
    bool all = false;                  // every installed level
    bool list = false;                 // print the installed levels and stop
    std::string outputDirectory;       // -f: the folder itself; installed levels: <this>/<id>

    bool separateDynamic = true;
    bool voxscripts = true;
    bool water = true;
    bool transparent = true;           // false: glass and water are left out (--no-transparent)
    bool physics = true;
    int blockResolution = 64;
    float scale = 10.0f;
    unsigned int threads = 0;

    bool preview = true;
    int previewWidth = 1600;
    vec3 previewFocus{0.0f};
    float previewRadius = 0.0f;

    // Maintenance modes, each instead of a conversion.
    std::vector<std::string> verify;   // folders to read back and check
    std::vector<std::string> rescale;  // factor, then folders
    std::string dumpVox;               // a .vox to describe
};

// Whether `path` is a Teardown level file: XML whose root element is <scene>.
bool isLevelFile(const std::string& path);

// Returns the process exit code.
int run(const Options& options);

} // namespace teardown

#endif // PROJECTV_TEARDOWN_IMPORT_H
