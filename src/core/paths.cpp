#include "core/paths.h"

#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
    #include <windows.h>
#elif defined(__APPLE__)
    #include <mach-o/dyld.h>
#endif

namespace projv::core {
    namespace {
        // The platform query, returning the path of the executable itself. Each platform has
        // exactly one reliable way to ask, and none of them are portable.
        std::filesystem::path executablePath() {
#if defined(_WIN32)
            // GetModuleFileNameW truncates rather than failing when the buffer is too small, and
            // reports the buffer size in that case instead of the length it wanted -- so grow
            // until the result is strictly shorter than what was offered.
            std::vector<wchar_t> buffer(MAX_PATH);
            for (;;) {
                DWORD written = GetModuleFileNameW(nullptr, buffer.data(), (DWORD)buffer.size());
                if (written == 0) {
                    throw std::runtime_error("executableDirectory: GetModuleFileNameW failed");
                }
                if (written < buffer.size()) {
                    return std::filesystem::path(std::wstring(buffer.data(), written));
                }
                if (buffer.size() >= 65536) {
                    throw std::runtime_error("executableDirectory: executable path is implausibly long");
                }
                buffer.resize(buffer.size() * 2);
            }
#elif defined(__APPLE__)
            // _NSGetExecutablePath reports the size it needs when the buffer is too small, so this
            // is at most two calls. The result may contain symlinks and '..'; the caller
            // canonicalizes.
            uint32_t size = 0;
            _NSGetExecutablePath(nullptr, &size);
            std::vector<char> buffer(size);
            if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
                throw std::runtime_error("executableDirectory: _NSGetExecutablePath failed");
            }
            return std::filesystem::path(buffer.data());
#else
            // /proc/self/exe is itself a symlink to the executable, so reading it also resolves
            // an invocation made through a symlinked name.
            std::error_code error;
            std::filesystem::path resolved = std::filesystem::read_symlink("/proc/self/exe", error);
            if (error) {
                throw std::runtime_error("executableDirectory: cannot read /proc/self/exe: " + error.message());
            }
            return resolved;
#endif
        }
    }

    std::filesystem::path executableDirectory() {
        std::filesystem::path path = executablePath();

        // Resolve symlinks and '..' so the result is stable regardless of how the binary was
        // invoked -- by bare name off PATH, by a relative path, or through a symlink. weakly_
        // canonical rather than canonical because it does not require every component to exist,
        // and a failure here should not be fatal when the raw path is already usable.
        std::error_code error;
        std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
        if (!error) {
            path = canonical;
        }

        std::filesystem::path directory = path.parent_path();
        if (directory.empty()) {
            throw std::runtime_error("executableDirectory: could not determine a parent directory for '" + path.string() + "'");
        }
        return directory;
    }
}
