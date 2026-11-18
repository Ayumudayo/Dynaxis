#pragma once

#include "admin/admin_http_server.hpp"
#include "admin/admin_types.hpp"
#include "admin/chat_ops_backend.hpp"
#include "admin/inventory_poller.hpp"

#include <memory>
#include <optional>
#include <string_view>

namespace dynaxis::admin {

/**
 * @brief HTTP 요청에 대한 인증/RBAC 판정 결과입니다.
 */
struct AdminAuthDecision {
    bool authorized{false};
    int http_status{401};
    AdminRole role{AdminRole::VIEWER};
    std::string error_code;
    std::string message;
};

/**
 * @brief 웹 관리 콘솔 v1/v2의 정적 UI, API v1 조회·제어 엔드포인트, 인가·읽기 전용 가드를 처리하는 라우터입니다.
 *
 * UI 버튼 숨김에 의존하지 않고 서버 측 핸들러에서 토큰 인증(`TOKEN`/`DEVELOPMENT`), 역할(`VIEWER`/`OPERATOR`/`ADMIN`),
 * `ADMIN_READ_ONLY` 차단을 직접 강제하며, 모든 JSON 응답·오류에서 비밀값을 배제합니다.
 */
class AdminRouter {
public:
    /**
     * @brief 설정, 인벤토리 폴러 참조, 선택적 v2 채팅 운영 백엔드를 받아 `AdminRouter`를 구성합니다.
     * @param config 인증 모드, 읽기 전용 여부, `web_root`, 딥링크 설정을 담은 `AdminConfig`
     * @param poller 최신 서비스 인벤토리 스냅샷을 제공하는 `InventoryPoller` 참조
     * @param chat_ops v2 채팅 방/사용자/제재/DLQ/감사 조회·제어를 수행할 백엔드 (미지정 시 503 응답)
     */
    AdminRouter(AdminConfig config,
                InventoryPoller& poller,
                std::shared_ptr<IChatOpsBackend> chat_ops = nullptr);

    /**
     * @brief 단일 HTTP 요청을 라우팅·인가하고 `AdminHttpResponse`를 생성합니다.
     * @param request 수신된 `AdminHttpRequest`
     * @return JSON API, 정적 자산, 또는 오류 응답(`401`/`403`/`404`/`405`/`409`)
     */
    AdminHttpResponse handle(const AdminHttpRequest& request) const;

    /**
     * @brief 요청 헤더의 Bearer / `X-Admin-Token`과 `AdminConfig`를 대조해 권한을 판정합니다.
     * @param request 검사할 `AdminHttpRequest`
     * @return 인가 여부, 역할(`VIEWER`/`OPERATOR`/`ADMIN`), 거부 시 상태 코드
     */
    AdminAuthDecision authorize_api_request(const AdminHttpRequest& request) const;

private:
    std::optional<AdminHttpResponse> try_serve_static(std::string_view path) const;
    AdminHttpResponse make_json(std::string status, std::string body) const;
    AdminHttpResponse make_error_json(std::string status, std::string_view code, std::string_view message) const;
    AdminHttpResponse from_chat_op(const ChatOpHttpResult& res) const;

    AdminConfig config_;
    InventoryPoller& poller_;
    std::shared_ptr<IChatOpsBackend> chat_ops_;
};

} // namespace dynaxis::admin
