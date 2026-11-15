import contextlib
import io
import struct
import unittest
from unittest import mock

import verify_soak_perf_gate as gate


class SoakPerfGateTests(unittest.TestCase):
    def test_fragmented_current_wire_frames_do_not_consume_next_header(self):
        first, second = b"hello-body", b"error-body"
        stream = io.BytesIO(struct.pack(">HHHII", len(first), 1, 0, 0x12345678, 0x11223344) + first
                            + struct.pack(">HHHII", len(second), 4, 0, 7, 9) + second)
        socket = mock.Mock()
        socket.recv.side_effect = lambda count: stream.read(min(count, 3))
        self.assertEqual(gate._read_frame(socket), (1, first))
        self.assertEqual(gate._read_frame(socket), (4, second))
        self.assertEqual(stream.read(), b"")

    def run_gate(self, failures, allowed_rate=0.0):
        def roundtrip(_worker, index):
            if index < failures:
                raise OSError("injected roundtrip failure")
            return 1.0

        with mock.patch.multiple(gate, WORKERS=1, REQUESTS_PER_WORKER=4,
                                 MAX_ERROR_RATE=allowed_rate, MIN_THROUGHPUT_RPS=1.0), \
                mock.patch.object(gate, "_single_connect_roundtrip", side_effect=roundtrip), \
                mock.patch.object(gate, "_fetch_metrics", return_value="chat_job_queue_depth 0\nchat_session_active 0\n"), \
                mock.patch.object(gate.time, "sleep"), \
                mock.patch.object(gate.time, "perf_counter", side_effect=[0.0, 1.0]), \
                contextlib.redirect_stdout(io.StringIO()):
            return gate.main()

    def test_request_failures_fail_even_when_success_latency_and_throughput_pass(self):
        self.assertEqual(self.run_gate(failures=2), 1)

    def test_clean_workload_passes(self):
        self.assertEqual(self.run_gate(failures=0), 0)

    def test_explicit_error_budget_is_honoured(self):
        self.assertEqual(self.run_gate(failures=2, allowed_rate=0.5), 0)
        self.assertEqual(self.run_gate(failures=3, allowed_rate=0.5), 1)

    def test_invalid_error_budget_is_rejected(self):
        for rate in (-0.1, 1.1, float("nan")):
            with self.subTest(rate=rate), mock.patch.object(gate, "MAX_ERROR_RATE", rate), \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(gate.main(), 1)


if __name__ == "__main__":
    unittest.main()
