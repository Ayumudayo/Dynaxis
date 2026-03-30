import unittest
from pathlib import Path


class GatewayPublicHeaderContractsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo_root = Path(__file__).resolve().parents[2]
        cls.header_path = cls.repo_root / "gateway" / "include" / "gateway" / "gateway_app.hpp"
        cls.header_text = cls.header_path.read_text(encoding="utf-8")
        cls.header_lines = cls.header_text.splitlines()

    def test_gateway_app_header_uses_single_impl_seam(self):
        self.assertIn("struct Impl;", self.header_text)
        self.assertIn("std::unique_ptr<Impl> impl_;", self.header_text)
        self.assertIn("friend struct GatewayAppAccess;", self.header_text)

    def test_gateway_app_header_does_not_expose_runtime_layout(self):
        forbidden_snippets = (
            "boost::asio::io_context io_;",
            "std::mutex session_mutex_;",
            "std::unordered_map<std::string, std::unique_ptr<SessionState>> sessions_;",
            "std::atomic<std::uint64_t> connections_total_{0};",
            "std::string udp_bind_secret_;",
            "std::shared_ptr<server::core::storage::redis::IRedisClient> redis_client_;",
            "std::atomic<bool> udp_enabled_{false};",
        )
        for snippet in forbidden_snippets:
            with self.subTest(snippet=snippet):
                self.assertNotIn(snippet, self.header_text)

    def test_gateway_app_header_does_not_expose_collaboration_api(self):
        forbidden_snippets = (
            "enum class IngressAdmission",
            "struct SelectedBackend",
            "struct CreatedBackendSession",
            "friend class GatewayConnection;",
            "friend class BackendConnection;",
            "std::string gateway_id() const;",
            "bool allow_anonymous() const noexcept;",
            "void record_connection_accept();",
            "void record_backend_resolve_fail();",
            "void record_backend_connect_fail();",
            "void record_backend_connect_timeout();",
            "void record_backend_write_error();",
            "void record_backend_send_queue_overflow();",
            "void record_backend_retry_scheduled();",
            "void record_backend_retry_budget_exhausted();",
            "IngressAdmission admit_ingress_connection();",
            "static const char* ingress_admission_name",
            "bool backend_circuit_allows_connect();",
            "void record_backend_connect_success_event();",
            "void record_backend_connect_failure_event();",
            "bool consume_backend_retry_budget();",
            "std::chrono::milliseconds backend_retry_delay",
            "std::uint32_t udp_bind_retry_delay_ms",
            "create_backend_connection(",
            "close_backend_connection(",
            "select_best_server(",
            "register_resume_routing_key(",
            "make_udp_bind_ticket_frame(",
            "try_send_direct_client_frame(",
            "on_backend_connected(",
            "make_resume_locator_key(",
            "load_resume_locator_selector(",
            "persist_resume_locator_hint(",
            "configure_gateway(",
            "configure_infrastructure(",
            "start_listener(",
            "start_udp_listener(",
            "stop_udp_listener(",
            "do_udp_receive(",
            "ParsedUdpBindRequest",
            "make_udp_bind_res_frame(",
            "trace_udp_bind_send(",
            "parse_udp_bind_req(",
            "apply_udp_bind_request(",
            "make_udp_bind_token(",
            "send_udp_datagram(",
            "#include <boost/asio/ip/udp.hpp>",
            "#include \"gateway/transport_session.hpp\"",
            "#include \"server/core/realtime/direct_bind.hpp\"",
            "#include \"server/core/discovery/instance_registry.hpp\"",
        )
        for snippet in forbidden_snippets:
            with self.subTest(snippet=snippet):
                self.assertNotIn(snippet, self.header_text)

    def test_gateway_app_header_stays_small_after_private_access_split(self):
        self.assertLess(
            len(self.header_lines),
            70,
            "gateway_app.hpp should stay narrow once the collaborator seam moves behind GatewayAppAccess",
        )


if __name__ == "__main__":
    unittest.main()
