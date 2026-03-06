#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace server::core::scripting {

/**
 * @brief Polling file watcher for script directories.
 *
 * `poll()` detects add/modify/remove changes via `last_write_time` snapshots.
 * If `lock_path` exists, polling is skipped to avoid reading files while an
 * external deploy step is in progress.
 */
class ScriptWatcher {
public:
    enum class ChangeKind {
        kAdded,
        kModified,
        kRemoved,
    };

    /** @brief Change event for one script file. */
    struct ChangeEvent {
        std::filesystem::path path;
        ChangeKind kind{ChangeKind::kModified};
    };

    using ChangeCallback = std::function<void(const ChangeEvent&)>;

    /** @brief Watcher configuration for paths, extensions, and lock policy. */
    struct Config {
        std::filesystem::path scripts_dir;
        std::vector<std::string> extensions;
        std::optional<std::filesystem::path> lock_path;
        bool recursive{false};
    };

    explicit ScriptWatcher(Config cfg);

    /**
     * @brief Poll script directory and invoke callback for detected changes.
     * @param on_change Callback executed on the caller thread.
     * @return true when polling succeeded (or skipped by lock), false on scan errors.
     */
    bool poll(const ChangeCallback& on_change);

private:
    /** @brief File snapshot captured at the last successful poll. */
    struct TrackedFile {
        std::filesystem::path path;
        std::filesystem::file_time_type mtime;
    };

    static bool file_exists(const std::filesystem::path& path);
    static std::string normalize_path_key(const std::filesystem::path& path);
    static std::string normalize_extension(std::string ext);

    bool matches_extension(const std::filesystem::path& path) const;
    bool scan_files(std::unordered_map<std::string, TrackedFile>& scanned) const;

    Config cfg_;
    std::unordered_set<std::string> extensions_;
    std::unordered_map<std::string, TrackedFile> tracked_;
};

} // namespace server::core::scripting
