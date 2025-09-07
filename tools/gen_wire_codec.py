#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import json
import sys
from pathlib import Path

HEADER_TMPL = """// 자동 생성 파일: tools/gen_wire_codec.py에 의해 생성됨
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "server/core/protocol.hpp"
#include "server/wire/v1/wire.pb.h"

namespace server { namespace wire { namespace codec {

// MsgId<T> 특수화: Protobuf 타입 -> msg_id
{msgid_specs}

// Encode helpers: Protobuf -> payload bytes
{encode_specs}

// Decode helpers: payload bytes -> Protobuf
{decode_specs}

// Utility: msg_id로 타입명을 얻기(디버그)
inline const char* TypeName(std::uint16_t id) {{
  switch (id) {{
{typename_cases}
    default: return "(unknown)";
  }}
}}

}}} // namespace server::wire::codec
"""

def parse_id(v: str) -> int:
    v = v.strip()
    return int(v, 16) if v.lower().startswith("0x") else int(v, 10)

def main():
    if len(sys.argv) != 3:
        print("usage: gen_wire_codec.py <wire_map.json> <out_header>")
        return 2
    spec_path = Path(sys.argv[1])
    out_header = Path(sys.argv[2])

    data = json.loads(spec_path.read_text(encoding="utf-8"))
    entries = data.get("entries", [])

    # sort by id
    items = []
    seen = set()
    for e in entries:
        mid = parse_id(e["id"]) 
        typ = e["type"].strip()
        name = e.get("name", typ.split("::")[-1])
        if mid in seen:
            raise ValueError(f"duplicated id 0x{mid:04X}")
        seen.add(mid)
        items.append((mid, typ, name))
    items.sort(key=lambda x: x[0])

    def ns(msg):
        # protocol.hpp uses namespace server::core::protocol
        return "server::core::protocol"

    msgid_specs = []
    encode_specs = []
    decode_specs = []
    typename_cases = []
    for mid, typ, name in items:
        msgid_specs.append(f"template<> inline constexpr std::uint16_t MsgId<{typ}>() {{ return {ns('') }::MSG_{'_' if False else ''}; }}")
        # The line above cannot get name from typ; build using provided mapping names
        # However protocol.hpp constant names are not derivable; so generate a direct constexpr mapping function instead.
    # Rebuild using a different approach (no template dependency on protocol constant names)
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

