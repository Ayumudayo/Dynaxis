#include "chat_hook_plugin_manager.hpp"

#include "server/core/util/log.hpp"

#include <array>
#include <system_error>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

/**
 * @brief 단일 chat-hook 플러그인 로드/호출/hot-reload 구현입니다.
 *
 * ABI 검증과 lock/sentinel, cache-copy를 결합해,
 * 배포 교체 중에도 서버 본체 안정성을 우선 보장합니다.
 */
namespace server::app::chat {

namespace corelog = server::core::log;

std::atomic<std::uint64_t> ChatHookPluginManager::g_cache_seq_{0};

namespace {

class SharedLibrary {
public:
    SharedLibrary() = default;
    SharedLibrary(const SharedLibrary&) = delete;
    SharedLibrary& operator=(const SharedLibrary&) = delete;

    ~SharedLibrary() {
        close();
    }

    bool open(const std::filesystem::path& path, std::string& error) {
        close();

#if defined(_WIN32)
        handle_ = ::LoadLibraryW(path.wstring().c_str());
        if (!handle_) {
            error = "LoadLibrary failed";
            return false;
        }
#else
        // dlerror는 thread-local 상태를 누적하므로 호출 전에 비워준다.
        (void)::dlerror();
        handle_ = ::dlopen(path.c_str(), RTLD_NOW);
        if (!handle_) {
            const char* msg = ::dlerror();
            error = msg ? msg : "dlopen failed";
            return false;
        }
#endif
        return true;
    }

    void close() {
#if defined(_WIN32)
        if (handle_) {
            ::FreeLibrary(handle_);
            handle_ = nullptr;
        }
#else
        if (handle_) {
            ::dlclose(handle_);
            handle_ = nullptr;
        }
#endif
    }

    void* symbol(const char* name, std::string& error) const {
        error.clear();
        if (!name || !*name) {
            error = "symbol name is empty";
            return nullptr;
        }

#if defined(_WIN32)
        if (!handle_) {
            error = "library not loaded";
            return nullptr;
        }
        FARPROC addr = ::GetProcAddress(handle_, name);
        if (!addr) {
            error = "GetProcAddress failed";
            return nullptr;
        }
        return reinterpret_cast<void*>(addr);
#else
        if (!handle_) {
            error = "library not loaded";
            return nullptr;
        }
        // dlsym 이전 dlerror 상태를 지워야 현재 심볼 조회 오류를 정확히 구분할 수 있다.
        (void)::dlerror();
        void* addr = ::dlsym(handle_, name);
        const char* msg = ::dlerror();
        if (msg != nullptr) {
            error = msg;
            return nullptr;
        }
        return addr;
#endif
    }

private:
#if defined(_WIN32)
    HMODULE handle_{nullptr};
#else
    void* handle_{nullptr};
#endif
};

static std::optional<std::filesystem::file_time_type> get_mtime(const std::filesystem::path& p) {
    std::error_code ec;
    auto t = std::filesystem::last_write_time(p, ec);
    if (ec) {
        return std::nullopt;
    }
    return t;
}

static bool file_exists(const std::filesystem::path& p) {
    std::error_code ec;
    const bool ok = std::filesystem::exists(p, ec);
    if (ec) {
        return false;
    }
    return ok;
}

static bool ensure_dir(const std::filesystem::path& p) {
    std::error_code ec;
    (void)std::filesystem::create_directories(p, ec);
    return !ec;
}

static std::filesystem::path make_default_lock_path(const std::filesystem::path& plugin_path) {
    auto dir = plugin_path.parent_path();
    auto stem = plugin_path.stem().string();
    return dir / (stem + "_LOCK");
}

static std::filesystem::path make_cache_path(const std::filesystem::path& cache_dir,
                                             const std::filesystem::path& plugin_path,
                                             std::uint64_t seq) {
    const auto stem = plugin_path.stem().string();
    const auto ext = plugin_path.extension().string();
    return cache_dir / (stem + "_" + std::to_string(seq) + ext);
}

static bool copy_to_cache(const std::filesystem::path& src,
                          const std::filesystem::path& dst,
                          std::string& error) {
    error.clear();

    std::error_code ec;
    auto tmp = dst;
    tmp += ".tmp";

    (void)std::filesystem::remove(tmp, ec);
    ec.clear();

    std::filesystem::copy_file(src, tmp, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        error = std::string("copy_file failed: ") + ec.message();
        return false;
    }

    ec.clear();
    std::filesystem::rename(tmp, dst, ec);
    if (ec) {
        // rename 실패(교차 볼륨/백신 간섭 등) 시 copy fallback으로 최대한 복구한다.
        std::error_code copy_ec;
        std::filesystem::copy_file(tmp, dst, std::filesystem::copy_options::overwrite_existing, copy_ec);
        std::error_code rm_ec;
        (void)std::filesystem::remove(tmp, rm_ec);
        if (copy_ec) {
            error = std::string("rename failed: ") + ec.message() + "; copy fallback failed: " + copy_ec.message();
            return false;
        }
    }

    return true;
}

} // namespace

struct ChatHookPluginManager::LoadedPlugin {
    SharedLibrary lib;
    const ChatHookApiV1* api{nullptr};
    void* instance{nullptr};
    std::filesystem::path cached_path;

    ~LoadedPlugin() {
        if (api && api->destroy) {
            try {
                api->destroy(instance);
            } catch (...) {
                // 플러그인 destroy 예외는 서버 종료/리로드 안정성을 해칠 수 있으므로 삼킨다.
            }
        }

        // 캐시 파일 삭제 전에 라이브러리를 명시적으로 unload한다.
        // 그렇지 않으면 Windows에서 파일 잠금 때문에 remove가 실패할 수 있다.
        lib.close();

        std::error_code ec;
        (void)std::filesystem::remove(cached_path, ec);
    }
};

ChatHookPluginManager::ChatHookPluginManager(Config cfg)
    : cfg_(std::move(cfg)) {
    if (cfg_.lock_path.has_value() && cfg_.lock_path->empty()) {
        cfg_.lock_path.reset();
    }
    if (cfg_.cache_dir.empty()) {
        std::error_code ec;
        cfg_.cache_dir = std::filesystem::temp_directory_path(ec) / "chat_hook_cache";
    }
    if (!cfg_.lock_path.has_value() && !cfg_.plugin_path.empty()) {
        cfg_.lock_path = make_default_lock_path(cfg_.plugin_path);
    }
}

void ChatHookPluginManager::poll_reload() {
    std::lock_guard<std::mutex> lock(reload_mu_);
    if (cfg_.plugin_path.empty()) {
        return;
    }
    if (!file_exists(cfg_.plugin_path)) {
        return;
    }

    if (cfg_.lock_path.has_value() && file_exists(*cfg_.lock_path)) {
        // 배포 스크립트가 lock을 올린 동안에는 reload를 보류해
        // 부분 복사된 바이너리를 읽는 사고를 피한다.
        return;
    }

    auto mtime = get_mtime(cfg_.plugin_path);
    if (!mtime.has_value()) {
        return;
    }

    if (last_attempt_mtime_.has_value() && *last_attempt_mtime_ == *mtime) {
        // 같은 mtime이면 이미 시도한 버전이므로 불필요한 재로드를 피한다.
        return;
    }

    reload_attempt_total_.fetch_add(1, std::memory_order_relaxed);
    last_attempt_mtime_ = *mtime;

    const auto record_failure = [this]() {
        reload_failure_total_.fetch_add(1, std::memory_order_relaxed);
    };

    if (!ensure_dir(cfg_.cache_dir)) {
        corelog::warn("chat_hook: failed to create cache dir: " + cfg_.cache_dir.string());
        record_failure();
        return;
    }

    const auto seq = ++g_cache_seq_;
    const auto cached = make_cache_path(cfg_.cache_dir, cfg_.plugin_path, seq);

    std::string copy_err;
    if (!copy_to_cache(cfg_.plugin_path, cached, copy_err)) {
        corelog::warn("chat_hook: cache copy failed: " + copy_err);
        record_failure();
        return;
    }

    auto mod = std::make_shared<LoadedPlugin>();
    mod->cached_path = cached;

    std::string open_err;
    if (!mod->lib.open(cached, open_err)) {
        corelog::warn("chat_hook: dlopen failed: " + open_err);
        record_failure();
        return;
    }

    std::string sym_err;
    void* sym = mod->lib.symbol("chat_hook_api_v1", sym_err);
    if (!sym) {
        corelog::warn("chat_hook: missing entrypoint chat_hook_api_v1: " + sym_err);
        record_failure();
        return;
    }

    // ABI 엔트리포인트는 C ABI 함수 포인터로 고정한다.
    // C++ name mangling 차이를 피하고 버전 체크를 단일 지점에서 수행하기 위함이다.
    using GetApiFn = const ChatHookApiV1* (CHAT_HOOK_CALL*)();
    auto get_api = reinterpret_cast<GetApiFn>(sym);
    const ChatHookApiV1* api = nullptr;
    try {
        api = get_api();
    } catch (...) {
        api = nullptr;
    }
    if (!api) {
        corelog::warn("chat_hook: plugin returned null api");
        record_failure();
        return;
    }
    if (api->abi_version != CHAT_HOOK_ABI_VERSION_V1) {
        corelog::warn("chat_hook: abi mismatch; expected v1");
        record_failure();
        return;
    }
    if (!api->on_chat_send) {
        corelog::warn("chat_hook: api.on_chat_send is null");
        record_failure();
        return;
    }

    void* instance = nullptr;
    if (api->create) {
        try {
            instance = api->create();
        } catch (...) {
            instance = nullptr;
        }
    }

    mod->api = api;
    mod->instance = instance;

    current_.store(std::move(mod), std::memory_order_release);
    reload_success_total_.fetch_add(1, std::memory_order_relaxed);
    corelog::info(std::string("chat_hook: loaded plugin name=") + (api->name ? api->name : "(null)") +
                  " version=" + (api->version ? api->version : "(null)"));
}

ChatHookPluginManager::Result ChatHookPluginManager::on_chat_send(std::uint32_t session_id,
                                                                   std::string_view room,
                                                                   std::string_view user,
                                                                   std::string_view text) const {
    Result result{};
    auto mod = current_.load(std::memory_order_acquire);
    if (!mod || !mod->api || !mod->api->on_chat_send) {
        return result;
    }

    ChatHookChatSendV1 in{};
    in.session_id = session_id;
    std::string room_s(room);
    std::string user_s(user);
    std::string text_s(text);
    in.room = room_s.c_str();
    in.user = user_s.c_str();
    in.text = text_s.c_str();

    // 플러그인 ABI는 호출자 제공 버퍼에 쓰는 방식이다.
    // 고정 크기 버퍼를 사용해 할당/해제 비용과 ABI 경계 복잡도를 줄인다.
    std::array<char, 512> notice_buf{};
    std::array<char, 1024> replace_buf{};
    ChatHookStrBufV1 notice_out{notice_buf.data(), static_cast<std::uint32_t>(notice_buf.size()), 0};
    ChatHookStrBufV1 replace_out{replace_buf.data(), static_cast<std::uint32_t>(replace_buf.size()), 0};
    ChatHookChatSendOutV1 out{notice_out, replace_out};

    ChatHookDecisionV1 decision = ChatHookDecisionV1::kPass;
    try {
        decision = mod->api->on_chat_send(mod->instance, &in, &out);
    } catch (const std::exception& ex) {
        corelog::warn(std::string("chat_hook: exception: ") + ex.what());
        decision = ChatHookDecisionV1::kPass;
    } catch (...) {
        corelog::warn("chat_hook: unknown exception");
        decision = ChatHookDecisionV1::kPass;
    }

    result.decision = decision;

    const auto clamp_and_assign = [](const ChatHookStrBufV1& b, std::string& out_str) {
        out_str.clear();
        if (!b.data || b.capacity == 0) {
            return;
        }
        // size가 잘못 보고되더라도 capacity-1을 상한으로 잘라
        // 플러그인 버그가 서버 메모리를 침범하지 않도록 방어한다.
        std::uint32_t n = b.size;
        if (n >= b.capacity) {
            n = b.capacity - 1;
        }
        out_str.assign(b.data, b.data + n);
    };

    clamp_and_assign(out.notice, result.notice);
    clamp_and_assign(out.replacement_text, result.replacement_text);
    return result;
}

ChatHookPluginManager::MetricsSnapshot ChatHookPluginManager::metrics_snapshot() const {
    MetricsSnapshot snap{};
    snap.plugin_path = cfg_.plugin_path;
    snap.reload_attempt_total = reload_attempt_total_.load(std::memory_order_relaxed);
    snap.reload_success_total = reload_success_total_.load(std::memory_order_relaxed);
    snap.reload_failure_total = reload_failure_total_.load(std::memory_order_relaxed);

    auto mod = current_.load(std::memory_order_acquire);
    if (!mod || !mod->api) {
        snap.loaded = false;
        return snap;
    }
    snap.loaded = true;
    if (mod->api->name) {
        snap.name = mod->api->name;
    }
    if (mod->api->version) {
        snap.version = mod->api->version;
    }
    return snap;
}

} // namespace server::app::chat
