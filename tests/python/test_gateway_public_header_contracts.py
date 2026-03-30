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

    def test_gateway_app_header_stays_small_after_impl_split(self):
        self.assertLess(
            len(self.header_lines),
            160,
            "gateway_app.hpp should shrink once the internal orchestration state moves behind Impl",
        )


if __name__ == "__main__":
    unittest.main()
