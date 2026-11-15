#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace admin_app {

/** @brief 내부 관측 endpoint의 HTTP 주소입니다. 인증 정보와 fragment는 허용하지 않습니다. */
struct HttpUrl {
    std::string host;
    std::uint16_t port{80};
    std::string target{"/"};
};

/** @brief HTTP 상태와 decoding된 본문을 함께 보존해 not-ready(503)도 관측합니다. */
struct HttpResponse {
    std::uint32_t status_code{0};
    std::string body;
};

/** @brief 단일 관리 조회의 전체 deadline과 decoding 후 본문 상한입니다. */
struct HttpProbeOptions {
    std::chrono::milliseconds timeout{1500};
    std::size_t body_limit{1024 * 1024};
};

/** @brief 내부 HTTP URL을 검증합니다. HTTPS/사용자정보/control 문자/잘못된 port는 거절합니다. */
std::optional<HttpUrl> parse_http_url(std::string_view raw);

/**
 * @brief 제한된 HTTP 조회를 수행합니다. network/framing/상한 실패는 null입니다.
 * @details DNS, connect, write, read에 하나의 deadline을 적용합니다. 4xx/5xx는 응답으로 반환합니다.
 */
std::optional<HttpResponse> http_get_response(const HttpUrl& url, HttpProbeOptions options = {});

/** @brief 200 응답의 본문만 반환합니다. 다른 상태나 조회 실패는 null입니다. */
std::optional<std::string> http_get_text(const HttpUrl& url, HttpProbeOptions options = {});

} // namespace admin_app
