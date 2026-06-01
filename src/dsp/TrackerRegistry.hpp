// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// Singleton plugin registry for ISweepTracker implementations. Header-only
/// because the registry is a small, allocation-light wrapper around @c dlopen
/// and a flat @c std::map of plugin entry points.

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
 * @brief Process-wide registry of available detector plugins.
 *
 * Supports two modes:
 *   - @c ECHOBOX_DYNAMIC_PLUGINS=ON : @c scanPlugins() loads @c .so files
 *     from a directory at startup (development workflow).
 *   - @c ECHOBOX_DYNAMIC_PLUGINS=OFF: @c registerBuiltin() takes the
 *     statically-linked detector's entry points (production workflow).
 *
 * Either way, @c createTracker(name) is the single entry point downstream
 * code (e.g. @c DspPipeline) uses to instantiate one.
 *
 * @note Singleton. Not thread-safe; populate it from one thread at startup,
 *       then treat it as read-only.
 */
class TrackerRegistry {
public:
    /// One registered plugin: a dlopen handle (or null for built-ins) plus
    /// its three C entry points.
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
     * @brief Scan a directory for @c .so plugins and register everything that
     *        exposes the three required C entry points.
     * @param path Directory to scan. Missing directory is non-fatal (logged).
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
     * @brief Register a statically-linked tracker (release / single-binary
     *        builds). No @c dlopen — entry points are resolved at link time.
     *
     * Idempotent: re-registering an already-known name is silently ignored.
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
     * @brief Create an instance of the named tracker.
     * @param name Tracker name as reported by its @c get_tracker_name() entry point.
     * @return Owned tracker with the plugin's @c destroy entry point as its
     *         deleter, or @c nullptr if @p name isn't registered.
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

    /// Names of every currently-registered tracker, in @c std::map order.
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
