#pragma once
/// @file
/// Small filesystem helpers. Header-only; used by the plugin loader to find
/// the @c ./algorithms directory next to the binary.

#include <string>
#include <filesystem>

#ifdef __linux__
#include <unistd.h>
#include <limits.h>
#endif

namespace fs = std::filesystem;

/// @brief Path-related helpers.
class PathUtils {
public:
    /**
     * @brief Absolute path to the directory containing the current executable.
     * @return Resolved directory on Linux; the current working directory as a
     *         fallback if the platform-specific resolution fails.
     */
    static fs::path getExecutableDir() {
#ifdef __linux__
        char result[PATH_MAX];
        ssize_t count = readlink("/proc/self/exe", result, PATH_MAX);
        if (count != -1) {
            fs::path exePath(std::string(result, count));
            return exePath.parent_path();
        }
#endif
        // Fallback to current working directory if resolution fails
        return fs::current_path();
    }
};
