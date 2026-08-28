// Prints projv::core::executableDirectory(). Used to check that the result is stable across
// how the binary was invoked: by relative path, by absolute path, from a foreign working
// directory, and through a symlink. All four must print the same real directory.
#include <cstdio>
#include "core/paths.h"

int main() {
    std::printf("%s\n", projv::core::executableDirectory().string().c_str());
    return 0;
}
