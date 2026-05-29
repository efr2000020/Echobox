#pragma once
#include <string>
#include <filesystem>

#ifdef __linux__
#include <unistd.h>
#include <limits.h>
#endif

namespace fs = std::filesystem;

class PathUtils {
public:
    /**
     * @brief Gets the absolute path to the directory containing the current executable.
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
