#pragma once

#include "admin/admin_types.hpp"

#include <string_view>
#include <vector>

namespace dynaxis::admin {

/**
 * @brief 환경설정과 토폴로지 JSON(`topology.active.json` / `topologies/default.json`)로부터
 *        신뢰 가능한 사전 등록 서비스 카탈로그를 생성합니다.
 *
 * @param config `AdminConfig` 설정(호스트/도커 타깃 모드, 포트, observability 활성 여부 포함)
 * @param out_warnings 토폴로지 파일을 읽지 못해 기본 `server-1..2`로 폴백한 경우 경고 목록
 * @return 전체 스택 서비스 대상 목록(`ServiceTargetConfig`)
 */
std::vector<ServiceTargetConfig> build_service_catalog(const AdminConfig& config,
                                                       std::vector<std::string>* out_warnings = nullptr);

/**
 * @brief `topology.json` 형식 문자열에서 채팅 `server` 인스턴스 목록을 파싱합니다.
 * @param json_text 토폴로지 JSON 본문
 * @param target_mode `"docker"`(컨테이너 호스트명·내부 포트 9090) 또는 `"host"`(루프백·`metrics_host_port`)
 * @param grafana_base_url Grafana 딥링크 생성용 기본 URL
 * @return 파싱된 채팅 서버 `ServiceTargetConfig` 목록 (실패 시 빈 벡터)
 */
std::vector<ServiceTargetConfig> parse_topology_servers_json(std::string_view json_text,
                                                             std::string_view target_mode,
                                                             std::string_view grafana_base_url);

} // namespace dynaxis::admin
