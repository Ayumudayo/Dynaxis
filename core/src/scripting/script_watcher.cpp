#include "server/core/scripting/script_watcher.hpp"

#include "server/core/util/log.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace server::core::scripting {

namespace {

int change_rank(ScriptWatcher::ChangeKind kind) {
    switch (kind) {
    case ScriptWatcher::ChangeKind::kAdded:
        return 0;
    case ScriptWatcher::ChangeKind::kModified:
        return 1;
    case ScriptWatcher::ChangeKind::kRemoved:
        return 2;
    }
    return 3;
}

} // namespace

ScriptWatcher::ScriptWatcher(Config cfg)
    : cfg_(std::move(cfg)) {
    if (cfg_.lock_path.has_value() && cfg_.lock_path->empty()) {
        cfg_.lock_path.reset();
    }

    for (const auto& ext : cfg_.extensions) {
        auto normalized = normalize_extension(ext);
        if (!normalized.empty()) {
            extensions_.insert(std::move(normalized));
        }
    }
}

bool ScriptWatcher::poll(const ChangeCallback& on_change) {
    if (cfg_.scripts_dir.empty()) {
        return true;
    }

    if (cfg_.lock_path.has_value() && file_exists(*cfg_.lock_path)) {
        return true;
    }

    std::unordered_map<std::string, TrackedFile> scanned;
    if (!scan_files(scanned)) {
        return false;
    }

    std::vector<ChangeEvent> changes;
    changes.reserve(scanned.size());

    for (const auto& [key, current] : scanned) {
        const auto it = tracked_.find(key);
        if (it == tracked_.end()) {
            changes.push_back(ChangeEvent{current.path, ChangeKind::kAdded});
            continue;
        }
        if (it->second.mtime != current.mtime) {
            changes.push_back(ChangeEvent{current.path, ChangeKind::kModified});
        }
    }

    for (const auto& [key, previous] : tracked_) {
        if (scanned.find(key) == scanned.end()) {
            changes.push_back(ChangeEvent{previous.path, ChangeKind::kRemoved});
        }
    }

    tracked_ = std::move(scanned);

    std::sort(changes.begin(), changes.end(), [](const ChangeEvent& lhs, const ChangeEvent& rhs) {
        const auto lhs_key = lhs.path.lexically_normal().generic_string();
        const auto rhs_key = rhs.path.lexically_normal().generic_string();
        if (lhs_key == rhs_key) {
            return change_rank(lhs.kind) < change_rank(rhs.kind);
        }
        return lhs_key < rhs_key;
    });

    if (!on_change) {
        return true;
    }
    for (const auto& change : changes) {
        on_change(change);
    }
    return true;
}

bool ScriptWatcher::file_exists(const std::filesystem::path& path) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) {
        return false;
    }
    return exists;
}

std::string ScriptWatcher::normalize_path_key(const std::filesystem::path& path) {
    auto key = path.lexically_normal().generic_string();
#if defined(_WIN32)
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
#endif
    return key;
}

std::string ScriptWatcher::normalize_extension(std::string ext) {
    if (ext.empty()) {
        return ext;
    }
    if (ext.front() != '.') {
        ext.insert(ext.begin(), '.');
    }
#if defined(_WIN32)
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
#endif
    return ext;
}

bool ScriptWatcher::matches_extension(const std::filesystem::path& path) const {
    if (extensions_.empty()) {
        return true;
    }
    const auto ext = normalize_extension(path.extension().string());
    return extensions_.find(ext) != extensions_.end();
}

bool ScriptWatcher::scan_files(std::unordered_map<std::string, TrackedFile>& scanned) const {
    scanned.clear();

    std::error_code ec;
    if (!std::filesystem::exists(cfg_.scripts_dir, ec) || ec) {
        server::core::log::warn(std::string("script_watcher: scripts_dir not found: ") + cfg_.scripts_dir.string());
        return false;
    }

    if (cfg_.recursive) {
        std::filesystem::recursive_directory_iterator it(cfg_.scripts_dir, ec);
        if (ec) {
            server::core::log::warn(std::string("script_watcher: failed to scan scripts_dir: ") + cfg_.scripts_dir.string());
            return false;
        }
        for (const auto& entry : it) {
            std::error_code st_ec;
            if (!entry.is_regular_file(st_ec) || st_ec) {
                continue;
            }
            const auto path = entry.path();
            if (!matches_extension(path)) {
                continue;
            }
            std::error_code mt_ec;
            const auto mtime = std::filesystem::last_write_time(path, mt_ec);
            if (mt_ec) {
                continue;
            }
            scanned.emplace(normalize_path_key(path), TrackedFile{path, mtime});
        }
        return true;
    }

    std::filesystem::directory_iterator it(cfg_.scripts_dir, ec);
    if (ec) {
        server::core::log::warn(std::string("script_watcher: failed to scan scripts_dir: ") + cfg_.scripts_dir.string());
        return false;
    }
    for (const auto& entry : it) {
        std::error_code st_ec;
        if (!entry.is_regular_file(st_ec) || st_ec) {
            continue;
        }
        const auto path = entry.path();
        if (!matches_extension(path)) {
            continue;
        }
        std::error_code mt_ec;
        const auto mtime = std::filesystem::last_write_time(path, mt_ec);
        if (mt_ec) {
            continue;
        }
        scanned.emplace(normalize_path_key(path), TrackedFile{path, mtime});
    }
    return true;
}

} // namespace server::core::scripting
