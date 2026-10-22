#ifndef PROJECTV_TEARDOWN_VERIFY_H
#define PROJECTV_TEARDOWN_VERIFY_H

#include <string>

namespace teardown {

struct VerifyResult {
    bool ok = false;
    size_t components = 0, files = 0, blocks = 0;
    size_t voxels = 0;                 // decoded, summed over every component that places them
    size_t chunks = 0, grids = 0, loose = 0; // what loadComposeFromDisk made
};

// Loads a converted folder through ProjectV and decodes every block. `ok` false on any problem.
VerifyResult verifyFolder(const std::string& folder, bool print = true);

} // namespace teardown

#endif // PROJECTV_TEARDOWN_VERIFY_H
