#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import json
import sys
from pathlib import Path

HEADER_TEMPLATE = """// 자동 생성 파일: tools/gen_opcodes.py에 의해 생성됨
#pragma once
#include <cstdint>

namespace {ns} {{
{body}
}} // namespace {ns}
"""

LINE_TEMPLATE = "static constexpr std::uint16_t {name:<24} = 0x{value:04X}; // {desc}"

def parse_id(v: str) -> int:
    v = v.strip()
    if v.lower().startswith("0x"):
        return int(v, 16)
    return int(v, 10)

def main():
    if len(sys.argv) != 3:
        print("usage: gen_opcodes.py <opcodes.json> <out_header>")
        return 2
    spec_path = Path(sys.argv[1])
    out_header = Path(sys.argv[2])

    data = json.loads(spec_path.read_text(encoding="utf-8"))
    ns = data.get("namespace", "server::core::protocol")
    items = data.get("opcodes", [])

    # 검증 및 정렬
    seen_ids = set()
    seen_names = set()
    parsed = []
    for it in items:
        name = it["name"].strip()
        val = parse_id(str(it["id"]))
        desc = it.get("desc", "")
        if name in seen_names:
            raise ValueError(f"중복 name: {name}")
        if val in seen_ids:
            raise ValueError(f"중복 id: 0x{val:04X}")
        seen_names.add(name)
        seen_ids.add(val)
        parsed.append((val, name, desc))
    parsed.sort(key=lambda x: x[0])

    lines = [LINE_TEMPLATE.format(name=name, value=val, desc=desc) for val, name, desc in parsed]
    body = "\n".join(lines)
    text = HEADER_TEMPLATE.format(ns=ns, body=body)

    out_header.parent.mkdir(parents=True, exist_ok=True)
    out_header.write_text(text + "\n", encoding="utf-8")
    print(f"[gen_opcodes] generated {out_header}")

if __name__ == "__main__":
    sys.exit(main())

