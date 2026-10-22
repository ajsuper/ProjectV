#ifndef PROJECTV_TEARDOWN_DISCOVER_H
#define PROJECTV_TEARDOWN_DISCOVER_H
// Finding Teardown and the levels installed with it.
//
// Looks in Steam's libraries -- the Flatpak Steam's first, then a native Steam's -- for the game,
// and lists every level file (an XML whose root is <scene>) at the top of each mod folder: the
// game's bundled mods, the DLCs, Workshop subscriptions and the user's local mods.
//
// The campaign levels ship only as compiled scene snapshots (data/bin/*.bin), not as level
// files, so they are not in the list.

#include <string>
#include <vector>

#include "level.h"

namespace teardown {

// The game folder (the one holding data/), or empty.
std::string findTeardown(const std::string& hint);

std::vector<LevelSource> findLevels(const std::string& teardownDir);

} // namespace teardown

#endif // PROJECTV_TEARDOWN_DISCOVER_H
