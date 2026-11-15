import hashlib
import hmac
import importlib.util
import json
from pathlib import Path
import unittest

source = Path(__file__).resolve().parents[2] / "tools" / "issue_chat_token.py"
spec = importlib.util.spec_from_file_location("chat_token_issuer", source)
issuer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(issuer)


class ChatTokenIssuerTests(unittest.TestCase):
    def test_schema_mac_and_unique_auth_sessions(self):
        key = "k" * 32
        first = issuer.issue_token("account-1", "표시 이름", key)
        second = issuer.issue_token("account-1", "표시 이름", key)
        data, signature = first.rsplit(".", 1)
        self.assertTrue(hmac.compare_digest(signature, hmac.new(key.encode(), data.encode(), hashlib.sha256).hexdigest()))
        payload = json.loads(bytes.fromhex(data.split(".")[1]))
        self.assertEqual(payload["sub"], "account-1")
        self.assertEqual(payload["name"], "표시 이름")
        self.assertEqual(payload["exp"] - payload["iat"], 900)
        self.assertNotEqual(payload["sid"], json.loads(bytes.fromhex(second.split(".")[1]))["sid"])

    def test_invalid_identity_key_and_lifetime(self):
        for subject, name, key, ttl in [("guest", "name", "k" * 32, 60), ("Account", "name", "k" * 32, 60),
                ("ok", "a\nb", "k" * 32, 60), ("ok", "name", "short", 60), ("ok", "name", "k" * 32, 86401)]:
            with self.assertRaises(ValueError):
                issuer.issue_token(subject, name, key, ttl)


if __name__ == "__main__":
    unittest.main()
