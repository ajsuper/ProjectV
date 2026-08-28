#ifndef PROJV_CORE_PATHS_H
#define PROJV_CORE_PATHS_H

#include <filesystem>

namespace projv::core {
    /**
    * The absolute path of the directory holding the running executable, with symlinks resolved.
    *
    * Every load function in ProjectV -- loadShader, loadRendererSpecification, loadComposeFromDisk
    * -- opens the path it is handed, resolving a relative one against the current working
    * directory. That is the correct behaviour for a load function, but it means an application
    * whose assets sit beside its binary has no way to find them unless it happens to have been
    * started from its own directory.
    *
    * This is the one piece of information the application cannot compute for itself. Everything
    * else is ordinary path composition:
    *
    *     const auto assets = projv::core::executableDirectory() / "assets";
    *     scene = projv::utils::loadComposeFromDisk((assets / "scenes/Castle").string());
    *
    * Several roots are just several paths -- there is no single "asset root" in the engine, and
    * nothing here changes how any other function behaves.
    *
    * @return The absolute, symlink-resolved directory containing the current executable.
    * @throws std::runtime_error if the platform query fails.
    */
    std::filesystem::path executableDirectory();
}

#endif
