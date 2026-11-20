"""오프라인 신뢰 issuer. 자격은 지정 파일에만 저장하고 stdout/log로 출력하지 않는다."""
from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import secrets
import time


def issue_token(subject: str, display_name: str, secret: str, ttl: int = 900) -> str:
    if not re.fullmatch(r"[a-z0-9_-]{1,64}", subject) or subject in {"guest", "anonymous"}:
        raise ValueError("invalid subject")
    if not display_name or len(display_name.encode()) > 128 or any(ord(c) < 32 or ord(c) == 127 for c in display_name):
        raise ValueError("invalid display name")
    if not 32 <= len(secret.encode()) <= 4096 or not 1 <= ttl <= 86400:
        raise ValueError("invalid key length or ttl")
    now = int(time.time())
    payload = {"v": 1, "iss": "dynaxis-auth", "aud": "dynaxis-chat", "sub": subject,
               "sid": secrets.token_hex(16), "name": display_name, "iat": now, "exp": now + ttl}
    data = "chat1." + json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode().hex()
    return data + "." + hmac.new(secret.encode(), data.encode(), hashlib.sha256).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--subject", required=True)
    parser.add_argument("--display-name", required=True)
    parser.add_argument("--ttl", type=int, default=900)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    token = issue_token(args.subject, args.display_name, os.environ.get("CHAT_AUTH_SIGNING_SECRET", ""), args.ttl)
    # 기존 파일은 덮어쓰지 않는다. Unix에서는 소유자만 읽을 수 있도록 생성한다.
    descriptor = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as output:
        output.write(token + "\n")
    print("Credential file created")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
