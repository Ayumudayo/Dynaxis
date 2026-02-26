#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import json
import sys
from pathlib import Path

HEADER_TMPL = """// 자동 생성 파일: tools/gen_wire_codec.py에 의해 생성됨
/**
 * @file
 * @brief Protobuf wire codec 자동 생성 헤더입니다.
 * @note tools/gen_wire_codec.py로 생성됩니다. 직접 수정하지 마세요.
 */
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "wire.pb.h"

namespace server {{ namespace wire {{ namespace codec {{

// MsgId<T> 템플릿 기본 정의
template<typename T>
constexpr std::uint16_t MsgId() {{ return 0; }}

// MsgId<T> 특수화: Protobuf 타입 -> msg_id
{msgid_specs}

// 인코드 헬퍼: Protobuf -> payload 바이트
{encode_specs}

// 디코드 헬퍼: payload 바이트 -> Protobuf
{decode_specs}

// 유틸리티: msg_id로 타입명을 얻기(디버그)
inline const char* TypeName(std::uint16_t id) {{
  switch (id) {{
{typename_cases}
    default: return "(unknown)";
  }}
}}

}}}}}} // namespace server::wire::codec
"""

def parse_id(v: str) -> int:
    v = v.strip()
    return int(v, 16) if v.lower().startswith("0x") else int(v, 10)

def main():
    if len(sys.argv) != 3:
        print("사용법: gen_wire_codec.py <wire_map.json> <out_header>")
        return 2
    spec_path = Path(sys.argv[1])
    out_header = Path(sys.argv[2])

    data = json.loads(spec_path.read_text(encoding="utf-8"))
    entries = data.get("entries", [])

    # ID 기준으로 정렬
    items = []
    seen = set()
    for e in entries:
        mid = parse_id(e["id"]) 
        typ = e["type"].strip()
        name = e.get("name", typ.split("::")[-1])
        if mid in seen:
            raise ValueError(f"중복 id 0x{mid:04X}")
        seen.add(mid)
        items.append((mid, typ, name))
    items.sort(key=lambda x: x[0])

    def ns(msg):
        # protocol.hpp는 server::core::protocol 네임스페이스를 사용합니다.
        return "server::core::protocol"

    msgid_specs = []
    encode_specs = []
    decode_specs = []
    typename_cases = []
    for mid, typ, name in items:
        msgid_specs.append(f"template<> inline constexpr std::uint16_t MsgId<{typ}>() {{ return {ns('') }::MSG_{'_' if False else ''}; }}")
        # 위 라인은 typ만으로 상수명을 역추론할 수 없어 매핑 기반으로 다시 구성합니다.
        # protocol.hpp 상수명은 자동 유도할 수 없으므로 직접 constexpr 매핑 함수를 생성합니다.
    # 다른 방식으로 다시 구성합니다(프로토콜 상수명 템플릿 의존 제거)
    msgid_specs.clear()
    for mid, typ, name in items:
        msgid_specs.append(f"template<> inline constexpr std::uint16_t MsgId<{typ}>() {{ return 0x{mid:04X}; }}")
        encode_specs.append( f"inline std::vector<std::uint8_t> Encode(const {typ}& m) {{ std::string bytes; m.SerializeToString(&bytes); return std::vector<std::uint8_t>(bytes.begin(), bytes.end()); }}" )
        decode_specs.append( f"inline bool Decode(const void* data, std::size_t size, {typ}& out) {{ return out.ParseFromArray(data, static_cast<int>(size)); }}" )
        typename_cases.append(f"    case 0x{mid:04X}: return \"{typ}\";")

    text = HEADER_TMPL.format(
        msgid_specs="\n".join(msgid_specs),
        encode_specs="\n".join(encode_specs),
        decode_specs="\n".join(decode_specs),
        typename_cases="\n".join(typename_cases),
    )
    out_header.parent.mkdir(parents=True, exist_ok=True)
    out_header.write_text(text + "\n", encoding="utf-8")
    print(f"[gen_wire_codec] generated {out_header}")

if __name__ == '__main__':
    sys.exit(main())
