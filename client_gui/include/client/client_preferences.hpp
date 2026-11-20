#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

/**
 * @brief 비비밀 로컬 클라이언트 환경설정(`access_token`·`resume_token` 제외)을 보관하는 구조체입니다.
 *
 * 재접속 시 자주 입력하는 호스트, 포트, 닉네임, UI 모드, 글자 배율, 차단 표시 여부,
 * 선택형 `Auto-Resume` 및 주기적 `Auto Ping` 설정만 저장하며 인증 자격은 절대 저장하지 않습니다.
 */
struct ClientPreferences {
    std::string host{"127.0.0.1"};
    int port{6000};
    std::string username{"guest"};
    bool workbench_mode{false};
    float text_zoom{1.0f};
    bool hide_blocked_messages{false};
    bool auto_resume{false};
    bool auto_ping{true};
};

/**
 * @brief CLI 인자에 의해 지정된 초기 화면 모드 재정의 값입니다.
 */
enum class CliModeOverride {
    NONE,
    MESSENGER,
    WORKBENCH
};

/**
 * @brief 실행 인자(`--host`, `--port`, `--user`, `--workbench`, `--smoke-test` 등) 파싱 결과입니다.
 */
struct ClientCliOptions {
    bool smoke_test{false};
    CliModeOverride mode_override{CliModeOverride::NONE};
    std::string host_override;
    int port_override{0};
    std::string user_override;
    int auto_resume_override{-1};
    std::filesystem::path config_path_override;
};

namespace detail {

inline std::string trim_ascii_whitespace(std::string_view sv) {
    std::size_t first = 0;
    while (first < sv.size() && std::isspace(static_cast<unsigned char>(sv[first]))) {
        ++first;
    }
    std::size_t last = sv.size();
    while (last > first && std::isspace(static_cast<unsigned char>(sv[last - 1]))) {
        --last;
    }
    return std::string(sv.substr(first, last - first));
}

inline std::string strip_control_chars_bounded(std::string_view sv, std::size_t max_bytes) {
    std::string out;
    out.reserve(std::min(sv.size(), max_bytes));
    for (unsigned char ch : sv) {
        if (ch < 0x20u || ch == 0x7Fu) {
            continue;
        }
        if (out.size() >= max_bytes) {
            break;
        }
        out.push_back(static_cast<char>(ch));
    }
    return trim_ascii_whitespace(out);
}

inline std::string ascii_lowercase_copy(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (unsigned char ch : sv) {
        out.push_back(static_cast<char>(std::tolower(ch)));
    }
    return out;
}

inline bool parse_bool_token(std::string_view raw, bool& out) {
    const std::string lower = ascii_lowercase_copy(trim_ascii_whitespace(raw));
    if (lower == "1" || lower == "true" || lower == "yes" || lower == "on") {
        out = true;
        return true;
    }
    if (lower == "0" || lower == "false" || lower == "no" || lower == "off") {
        out = false;
        return true;
    }
    return false;
}

inline bool parse_int_strict(std::string_view raw, int& out) {
    const std::string cleaned = trim_ascii_whitespace(raw);
    if (cleaned.empty()) {
        return false;
    }
    char* end = nullptr;
    const long val = std::strtol(cleaned.c_str(), &end, 10);
    if (end == cleaned.c_str() || *end != '\0') {
        return false;
    }
    if (val < 1 || val > 65535) {
        return false;
    }
    out = static_cast<int>(val);
    return true;
}

inline bool parse_float_strict(std::string_view raw, float& out) {
    const std::string cleaned = trim_ascii_whitespace(raw);
    if (cleaned.empty()) {
        return false;
    }
    char* end = nullptr;
    const float val = std::strtof(cleaned.c_str(), &end);
    if (end == cleaned.c_str() || *end != '\0') {
        return false;
    }
    out = val;
    return true;
}

inline std::filesystem::path env_path(const char* name) {
    if (const char* raw = std::getenv(name)) {
        if (raw[0] != '\0') {
            return std::filesystem::path(raw);
        }
    }
    return {};
}

} // namespace detail

/**
 * @brief 환경설정 필드의 유효 범위와 제어문자 제거 규칙을 적용해 정규화합니다.
 * @param prefs 정규화할 `ClientPreferences` 참조
 */
inline void sanitize_client_preferences(ClientPreferences& prefs) {
    prefs.host = detail::strip_control_chars_bounded(prefs.host, 127);
    if (prefs.host.empty()) {
        prefs.host = "127.0.0.1";
    }
    prefs.username = detail::strip_control_chars_bounded(prefs.username, 127);
    if (prefs.username.empty()) {
        prefs.username = "guest";
    }
    if (prefs.port < 1 || prefs.port > 65535) {
        prefs.port = 6000;
    }
    prefs.text_zoom = std::clamp(prefs.text_zoom, 0.85f, 1.40f);
}

/**
 * @brief 비비밀 로컬 환경설정을 안전한 `key=value` 텍스트 형식으로 직렬화합니다.
 * @param prefs 직렬화할 `ClientPreferences`
 * @return 디스크 저장용 설정 텍스트 문자열
 */
inline std::string serialize_client_preferences(const ClientPreferences& prefs) {
    ClientPreferences clean = prefs;
    sanitize_client_preferences(clean);

    std::ostringstream oss;
    oss << "# Dynaxis client non-secret preferences (tokens are never stored)\n";
    oss << "host=" << clean.host << '\n';
    oss << "port=" << clean.port << '\n';
    oss << "username=" << clean.username << '\n';
    oss << "ui_mode=" << (clean.workbench_mode ? "workbench" : "messenger") << '\n';
    oss << std::fixed << std::setprecision(2) << "text_zoom=" << clean.text_zoom << '\n';
    oss << "hide_blocked_messages=" << (clean.hide_blocked_messages ? "1" : "0") << '\n';
    oss << "auto_resume=" << (clean.auto_resume ? "1" : "0") << '\n';
    oss << "auto_ping=" << (clean.auto_ping ? "1" : "0") << '\n';
    return oss.str();
}

/**
 * @brief `key=value` 설정 본문을 파싱해 `ClientPreferences`에 반영합니다.
 *
 * 비밀 키(`access_token`, `resume_token`, `password`)는 파일에 임의로 추가되어 있더라도 무시합니다.
 *
 * @param content 설정 파일 텍스트 본문
 * @param out_prefs 파싱 결과를 덮어쓸 `ClientPreferences` 참조
 */
inline void parse_client_preferences(std::string_view content, ClientPreferences& out_prefs) {
    std::size_t pos = 0;
    while (pos <= content.size()) {
        const std::size_t nl = content.find('\n', pos);
        const std::string_view raw_line =
            (nl == std::string_view::npos) ? content.substr(pos) : content.substr(pos, nl - pos);
        const std::string line = detail::trim_ascii_whitespace(raw_line);
        if (!line.empty() && line[0] != '#') {
            const std::size_t eq = line.find('=');
            if (eq != std::string::npos) {
                const std::string key = detail::ascii_lowercase_copy(detail::trim_ascii_whitespace(std::string_view(line).substr(0, eq)));
                const std::string val = detail::trim_ascii_whitespace(std::string_view(line).substr(eq + 1));

                if (key == "access_token" || key == "resume_token" || key == "password") {
                    // 비밀 자격은 로컬 설정 파일로부터 절대 로드하지 않습니다.
                } else if (key == "host") {
                    out_prefs.host = val;
                } else if (key == "port") {
                    int parsed_port = 0;
                    if (detail::parse_int_strict(val, parsed_port)) {
                        out_prefs.port = parsed_port;
                    }
                } else if (key == "username" || key == "user") {
                    out_prefs.username = val;
                } else if (key == "ui_mode" || key == "mode") {
                    const std::string mode_val = detail::ascii_lowercase_copy(val);
                    if (mode_val == "workbench") {
                        out_prefs.workbench_mode = true;
                    } else if (mode_val == "messenger") {
                        out_prefs.workbench_mode = false;
                    }
                } else if (key == "workbench_mode") {
                    bool b = false;
                    if (detail::parse_bool_token(val, b)) {
                        out_prefs.workbench_mode = b;
                    }
                } else if (key == "text_zoom") {
                    float z = 1.0f;
                    if (detail::parse_float_strict(val, z)) {
                        out_prefs.text_zoom = z;
                    }
                } else if (key == "hide_blocked_messages") {
                    bool b = false;
                    if (detail::parse_bool_token(val, b)) {
                        out_prefs.hide_blocked_messages = b;
                    }
                } else if (key == "auto_resume") {
                    bool b = false;
                    if (detail::parse_bool_token(val, b)) {
                        out_prefs.auto_resume = b;
                    }
                } else if (key == "auto_ping") {
                    bool b = true;
                    if (detail::parse_bool_token(val, b)) {
                        out_prefs.auto_ping = b;
                    }
                }
            }
        }
        if (nl == std::string_view::npos) {
            break;
        }
        pos = nl + 1;
    }
    sanitize_client_preferences(out_prefs);
}

/**
 * @brief 현재 플랫폼과 환경변수(`DYNAXIS_CLIENT_CONFIG`)에 따른 기본 설정 파일 경로를 계산합니다.
 * @return 비비밀 설정 파일을 읽고 쓸 기본 경로 (환경이 불완전하면 빈 경로)
 */
inline std::filesystem::path default_client_preferences_path() {
    if (const auto custom = detail::env_path("DYNAXIS_CLIENT_CONFIG"); !custom.empty()) {
        return custom;
    }
#if defined(_WIN32)
    if (auto base = detail::env_path("LOCALAPPDATA"); !base.empty()) {
        return base / "Dynaxis" / "client_gui_prefs.cfg";
    }
    if (auto fallback = detail::env_path("APPDATA"); !fallback.empty()) {
        return fallback / "Dynaxis" / "client_gui_prefs.cfg";
    }
    return {};
#else
    if (auto xdg = detail::env_path("XDG_CONFIG_HOME"); !xdg.empty()) {
        return xdg / "dynaxis" / "client_gui_prefs.cfg";
    }
    if (auto home = detail::env_path("HOME"); !home.empty()) {
        return home / ".config" / "dynaxis" / "client_gui_prefs.cfg";
    }
    return {};
#endif
}

/**
 * @brief 지정 경로의 설정 파일에서 `ClientPreferences`를 읽어 들입니다.
 * @param path 읽을 설정 파일 경로
 * @param out_prefs 읽은 값을 저장할 구조체 참조
 * @return 파일을 열어 파싱했으면 true, 파일이 없거나 열 수 없으면 false
 */
inline bool load_client_preferences_file(const std::filesystem::path& path, ClientPreferences& out_prefs) {
    if (path.empty()) {
        return false;
    }
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in.is_open()) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    parse_client_preferences(ss.str(), out_prefs);
    return true;
}

/**
 * @brief 비비밀 `ClientPreferences`를 지정 경로 파일에 저장합니다.
 * @param path 저장할 설정 파일 경로
 * @param prefs 저장할 환경설정 구조체
 * @return 저장에 성공하면 true, 실패하거나 경로가 비어 있으면 false
 */
inline bool save_client_preferences_file(const std::filesystem::path& path, const ClientPreferences& prefs) {
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return false;
        }
    }
    std::ofstream out(path, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!out.is_open()) {
        return false;
    }
    const std::string body = serialize_client_preferences(prefs);
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
    return out.good();
}

/**
 * @brief 명령줄 인자(`argc`, `argv`)를 파싱해 클라이언트 실행 옵션을 추출합니다.
 * @param argc 인자 개수
 * @param argv 인자 문자열 배열
 * @return 추출된 `ClientCliOptions`
 */
inline ClientCliOptions parse_client_cli_args(int argc, const char* const* argv) {
    ClientCliOptions opts;
    if (argc <= 1 || argv == nullptr) {
        return opts;
    }
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == nullptr) {
            continue;
        }
        const std::string_view arg(argv[i]);
        auto take_value = [&](std::string_view flag, std::string_view& out_val) -> bool {
            if (arg == flag && (i + 1) < argc && argv[i + 1] != nullptr) {
                out_val = std::string_view(argv[++i]);
                return true;
            }
            if (arg.size() > flag.size() && arg.rfind(flag, 0) == 0 && arg[flag.size()] == '=') {
                out_val = arg.substr(flag.size() + 1);
                return true;
            }
            return false;
        };

        std::string_view val;
        if (arg == "--smoke-test") {
            opts.smoke_test = true;
        } else if (arg == "--workbench") {
            opts.mode_override = CliModeOverride::WORKBENCH;
        } else if (arg == "--messenger") {
            opts.mode_override = CliModeOverride::MESSENGER;
        } else if (arg == "--auto-resume") {
            opts.auto_resume_override = 1;
        } else if (arg == "--no-auto-resume") {
            opts.auto_resume_override = 0;
        } else if (take_value("--host", val)) {
            opts.host_override = detail::strip_control_chars_bounded(val, 127);
        } else if (take_value("--port", val)) {
            int parsed = 0;
            if (detail::parse_int_strict(val, parsed)) {
                opts.port_override = parsed;
            }
        } else if (take_value("--user", val)) {
            opts.user_override = detail::strip_control_chars_bounded(val, 127);
        } else if (take_value("--config", val)) {
            const std::string cleaned = detail::trim_ascii_whitespace(val);
            if (!cleaned.empty()) {
                opts.config_path_override = std::filesystem::path(cleaned);
            }
        }
    }
    return opts;
}

/**
 * @brief 파싱된 CLI 인자 재정의 값을 파일에서 로드된 환경설정에 덮어씁니다.
 * @param prefs 덮어쓸 `ClientPreferences` 참조
 * @param cli 적용할 `ClientCliOptions`
 */
inline void apply_cli_overrides(ClientPreferences& prefs, const ClientCliOptions& cli) {
    if (!cli.host_override.empty()) {
        prefs.host = cli.host_override;
    }
    if (cli.port_override > 0) {
        prefs.port = cli.port_override;
    }
    if (!cli.user_override.empty()) {
        prefs.username = cli.user_override;
    }
    if (cli.mode_override == CliModeOverride::WORKBENCH) {
        prefs.workbench_mode = true;
    } else if (cli.mode_override == CliModeOverride::MESSENGER) {
        prefs.workbench_mode = false;
    }
    if (cli.auto_resume_override == 1) {
        prefs.auto_resume = true;
    } else if (cli.auto_resume_override == 0) {
        prefs.auto_resume = false;
    }
    sanitize_client_preferences(prefs);
}

/**
 * @brief UNIX 만료 시각(ms)과 현재 시각(ms)의 차이를 읽기 쉬운 남은 시간 문자열로 변환합니다.
 * @param expires_unix_ms 만료 예정 UNIX 타임스탬프(ms). `0`이면 미지정으로 간주
 * @param now_unix_ms 기준 현재 UNIX 타임스탬프(ms)
 * @return 미지정 시 `"-"`, 만료 시 `"Expired"`, 유효 시 `"Xh Ym Zs"` / `"Ym Zs"` / `"Zs"`
 */
inline std::string format_remaining_ttl(std::uint64_t expires_unix_ms, std::uint64_t now_unix_ms) {
    if (expires_unix_ms == 0) {
        return "-";
    }
    if (now_unix_ms >= expires_unix_ms) {
        return "Expired";
    }
    const std::uint64_t remaining_sec = (expires_unix_ms - now_unix_ms + 999ULL) / 1000ULL;
    const std::uint64_t hours = remaining_sec / 3600ULL;
    const std::uint64_t minutes = (remaining_sec % 3600ULL) / 60ULL;
    const std::uint64_t seconds = remaining_sec % 60ULL;
    char buf[48]{};
    if (hours > 0) {
        std::snprintf(
            buf,
            sizeof(buf),
            "%lluh %02llum %02llus",
            static_cast<unsigned long long>(hours),
            static_cast<unsigned long long>(minutes),
            static_cast<unsigned long long>(seconds));
    } else if (minutes > 0) {
        std::snprintf(
            buf,
            sizeof(buf),
            "%llum %02llus",
            static_cast<unsigned long long>(minutes),
            static_cast<unsigned long long>(seconds));
    } else {
        std::snprintf(buf, sizeof(buf), "%llus", static_cast<unsigned long long>(seconds));
    }
    return std::string(buf);
}
