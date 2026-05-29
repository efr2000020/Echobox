#pragma once
#include "ISweepTracker.hpp"
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <map>
#include <iostream>
#include <filesystem>
#include <dlfcn.h>

namespace fs = std::filesystem;

/**
 * @brief Dynamic Plugin Registry for DSP algorithms.
 * Scans a directory for .so files and loads them.
 */
class TrackerRegistry {
public:
    struct Plugin {
        void* handle;
        CreateTrackerFunc create;
        DestroyTrackerFunc destroy;
        std::string name;
    };

    static TrackerRegistry& getInstance() {
        static TrackerRegistry instance;
        return instance;
    }

    /**
     * @brief Scans a directory for algorithm plugins.
     * @param path The directory to scan.
     */
    void scanPlugins(const std::string& path) {
        if (!fs::exists(path)) {
            std::cerr << "[Registry] Plugin path does not exist: " << path << "\n";
            return;
        }

        for (const auto& entry : fs::directory_iterator(path)) {
            if (entry.is_regular_file() && entry.path().extension() == ".so") {
                loadPlugin(entry.path().string());
            }
        }
    }

    /**
     * @brief Registers a statically-linked tracker (release / single-binary
     * builds). No dlopen — the entry points are resolved at link time.
     */
    void registerBuiltin(CreateTrackerFunc create,
                         DestroyTrackerFunc destroy,
                         GetTrackerNameFunc getName) {
        std::string name = getName();
        if (!m_plugins.count(name)) {
            m_plugins[name] = {nullptr, create, destroy, name};
        }
    }

    /**
     * @brief Creates an instance of a tracker by name.
     */
    std::unique_ptr<ISweepTracker, std::function<void(ISweepTracker*)>> createTracker(const std::string& name) {
        auto it = m_plugins.find(name);
        if (it != m_plugins.end()) {
            ISweepTracker* tracker = it->second.create();
            auto destroy = it->second.destroy;
            return std::unique_ptr<ISweepTracker, std::function<void(ISweepTracker*)>>(tracker, destroy);
        }
        return nullptr;
    }

    std::vector<std::string> getAvailableAlgorithms() const {
        std::vector<std::string> names;
        for (const auto& [name, _] : m_plugins) {
            names.push_back(name);
        }
        return names;
    }

    ~TrackerRegistry() {
        m_plugins.clear();
        for (void* handle : m_handles) {
            dlclose(handle);
        }
    }

private:
    TrackerRegistry() = default;

    void loadPlugin(const std::string& path) {
        void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            std::cerr << "[Registry] Failed to load plugin " << path << ": " << dlerror() << "\n";
            return;
        }

        auto getName = (GetTrackerNameFunc)dlsym(handle, "get_tracker_name");
        auto create = (CreateTrackerFunc)dlsym(handle, "create_tracker");
        auto destroy = (DestroyTrackerFunc)dlsym(handle, "destroy_tracker");

        if (!getName || !create || !destroy) {
            std::cerr << "[Registry] Plugin " << path << " missing required entry points.\n";
            dlclose(handle);
            return;
        }

        std::string name = getName();
        if (m_plugins.count(name)) {
            std::cerr << "[Registry] Algorithm " << name << " already registered. Skipping " << path << "\n";
            dlclose(handle);
            return;
        }

        m_plugins[name] = {handle, create, destroy, name};
        m_handles.push_back(handle);
    }

    std::map<std::string, Plugin> m_plugins;
    std::vector<void*> m_handles;
};
