#include "chat_hook_plugin_chain.hpp"

#include "server/core/util/log.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <unordered_set>

/**
 * @brief 다중 chat-hook 플러그인 체인 재구성/적용 구현입니다.
 *
 * 디렉터리 스캔 오류 시 기존 체인을 유지해 가용성을 우선하고,
 * hot-reload는 안전한 cache-copy 경로를 통해 진행합니다.
 */
namespace server::app::chat {

namespace corelog = server::core::log;

ChatHookPluginChain::ChatHookPluginChain(Config cfg)
    : cfg_(std::move(cfg)) {
    if (cfg_.plugins_dir.has_value() && cfg_.plugins_dir->empty()) {
        cfg_.plugins_dir.reset();
    }
    if (cfg_.single_lock_path.has_value() && cfg_.single_lock_path->empty()) {
        cfg_.single_lock_path.reset();
    }
}

std::string ChatHookPluginChain::module_extension() {
#if defined(_WIN32)
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

std::string ChatHookPluginChain::normalize_key(const std::filesystem::path& p) {
    // 키 정규화는 "빠르고 실패가 적은" 연산을 우선한다.
    // weakly_canonical()은 파일 존재 여부/시스템 호출에 영향을 받아
    // 배포 교체 타이밍에 불필요한 실패를 만들 수 있어 사용하지 않는다.
    auto s = p.lexically_normal().generic_string();
#if defined(_WIN32)
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
#endif
    return s;
}

bool ChatHookPluginChain::get_desired_paths(std::vector<std::filesystem::path>& out) const {
    out.clear();

    if (!cfg_.plugin_paths.empty()) {
        out.reserve(cfg_.plugin_paths.size());
        for (const auto& p : cfg_.plugin_paths) {
            if (!p.empty()) {
                out.push_back(p);
            }
        }
        return true;
    }

    if (!cfg_.plugins_dir.has_value()) {
        return true;
    }

    const auto ext = module_extension();

    std::error_code ec;
    std::filesystem::directory_iterator it(*cfg_.plugins_dir, ec);
    if (ec) {
        corelog::warn(std::string("chat_hook: failed to scan plugins dir: ") + cfg_.plugins_dir->string());
        return false;
    }

    for (const auto& entry : it) {
        std::error_code st_ec;
        if (!entry.is_regular_file(st_ec) || st_ec) {
            continue;
        }

        const auto p = entry.path();
        if (p.extension().string() != ext) {
            continue;
        }

        out.push_back(p);
    }

    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.filename().string() < b.filename().string();
    });
    return true;
}

void ChatHookPluginChain::poll_reload() {
    const bool configured = (!cfg_.plugin_paths.empty() || cfg_.plugins_dir.has_value());
    if (!configured) {
        return;
    }

    std::vector<std::filesystem::path> paths;
    if (!get_desired_paths(paths)) {
        // 디렉터리 스캔 실패 시 체인을 즉시 비우지 않는다.
        // 일시적인 FS 오류(권한/잠금/배포 교체 순간)에서도
        // 기존 로드 플러그인으로 서비스 기능을 최대한 유지하기 위함이다.
        auto ordered = ordered_.load(std::memory_order_acquire);
        if (ordered) {
            for (auto& mgr : *ordered) {
                if (mgr) {
                    mgr->poll_reload();
                }
            }
        }
        return;
    }

    if (paths.empty()) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            by_key_.clear();
        }
        ordered_.store(std::make_shared<PluginList>(), std::memory_order_release);
        return;
    }

    // 단일 플러그인 모드 + lock 경로 지정 시에는 해당 lock 규칙을 강제한다.
    const bool single_explicit = (!cfg_.plugins_dir.has_value() && paths.size() == 1 && cfg_.single_lock_path.has_value());

    std::shared_ptr<PluginList> ordered = std::make_shared<PluginList>();
    ordered->reserve(paths.size());

    std::unordered_set<std::string> keep;
    keep.reserve(paths.size());

    {
        std::lock_guard<std::mutex> lock(mu_);

        for (const auto& p : paths) {
            const auto key = normalize_key(p);
            keep.insert(key);

            auto it = by_key_.find(key);
            if (it == by_key_.end()) {
                ChatHookPluginManager::Config cfg;
                cfg.plugin_path = p;
                cfg.cache_dir = cfg_.cache_dir;
                if (single_explicit) {
                    cfg.lock_path = *cfg_.single_lock_path;
                }
                auto mgr = std::make_shared<ChatHookPluginManager>(std::move(cfg));
                it = by_key_.emplace(key, std::move(mgr)).first;
            }

            ordered->push_back(it->second);
        }

        // 현재 구성에 없는 엔트리는 맵에서 제거한다.
        // shared_ptr가 남아 있으면 즉시 파괴되지 않아 진행 중 호출의 안전성을 보존한다.
        for (auto it = by_key_.begin(); it != by_key_.end(); ) {
            if (keep.count(it->first) == 0) {
                it = by_key_.erase(it);
            } else {
                ++it;
            }
        }
    }

    ordered_.store(ordered, std::memory_order_release);

    for (auto& mgr : *ordered) {
        if (mgr) {
            mgr->poll_reload();
        }
    }
}

ChatHookPluginChain::Outcome ChatHookPluginChain::on_chat_send(std::uint32_t session_id,
                                                               std::string_view room,
                                                               std::string_view user,
                                                               std::string& text) const {
    Outcome out{};

    auto ordered = ordered_.load(std::memory_order_acquire);
    if (!ordered) {
        return out;
    }

    // 각 플러그인 정책:
    // - kPass: 다음 플러그인으로 진행
    // - kReplaceText: text를 대체하고 다음 플러그인으로 진행
    // - kHandled/kBlock: 기본 채팅 경로를 중단하고 즉시 반환
    for (const auto& mgr : *ordered) {
        if (!mgr) {
            continue;
        }

        const auto r = mgr->on_chat_send(session_id, room, user, text);
        if (!r.notice.empty()) {
            out.notices.push_back(r.notice);
        }

        switch (r.decision) {
        case ChatHookDecisionV1::kPass:
            break;
        case ChatHookDecisionV1::kHandled:
            out.stop_default = true;
            return out;
        case ChatHookDecisionV1::kReplaceText:
            if (!r.replacement_text.empty()) {
                text = r.replacement_text;
            }
            break;
        case ChatHookDecisionV1::kBlock:
            out.stop_default = true;
            return out;
        default:
            break;
        }
    }

    return out;
}

ChatHookPluginChain::MetricsSnapshot ChatHookPluginChain::metrics_snapshot() const {
    MetricsSnapshot out{};
    out.configured = (!cfg_.plugin_paths.empty() || cfg_.plugins_dir.has_value());

    if (!out.configured) {
        out.mode = "none";
        return out;
    }

    if (cfg_.plugins_dir.has_value()) {
        out.mode = "dir";
    } else if (!cfg_.plugin_paths.empty()) {
        if (cfg_.plugin_paths.size() == 1 && cfg_.single_lock_path.has_value()) {
            out.mode = "single";
        } else {
            out.mode = "paths";
        }
    } else {
        out.mode = "none";
    }

    auto ordered = ordered_.load(std::memory_order_acquire);
    if (!ordered) {
        return out;
    }

    out.plugins.reserve(ordered->size());
    for (const auto& mgr : *ordered) {
        if (!mgr) {
            continue;
        }
        out.plugins.push_back(mgr->metrics_snapshot());
    }

    return out;
}

} // namespace server::app::chat
