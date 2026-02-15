#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import json
import sys
from pathlib import Path
from typing import Any

HEADER_TEMPLATE = """// 자동 생성 파일: tools/gen_opcodes.py에 의해 생성됨
#pragma once
#include <cstdint>
#include <string_view>

namespace {ns} {{
{body}
}} // namespace {ns}
"""

LINE_TEMPLATE = "static constexpr std::uint16_t {name:<24} = 0x{value:04X}; // {desc}"

NAME_FUNC_TEMPLATE = """

inline constexpr std::string_view opcode_name( std::uint16_t id ) noexcept
{{
  switch( id )
  {{
{cases}
    default: return std::string_view{{}};
  }}
}}
""".lstrip("\n")

def parse_id(v: Any) -> int:
    if isinstance(v, int):
        return v
    s = str(v).strip()
    if s.lower().startswith("0x"):
        return int(s, 16)
    return int(s, 10)


def parse_groups(data: dict):
    raw = data.get("groups")
    if not raw:
        return [], {}

    order = []
    by_name = {}
    for g in raw:
        name = str(g.get("name", "")).strip()
        if not name:
            raise ValueError("group.name is required")
        if name in by_name:
            raise ValueError(f"duplicate group: {name}")

        id_min = parse_id(g.get("id_min"))
        id_max = parse_id(g.get("id_max"))
        if not (0 <= id_min <= 0xFFFF and 0 <= id_max <= 0xFFFF and id_min <= id_max):
            raise ValueError(f"invalid group range: {name} [0x{id_min:04X}..0x{id_max:04X}]")

        desc = str(g.get("desc", "")).strip()
        order.append(name)
        by_name[name] = {
            "id_min": id_min,
            "id_max": id_max,
            "desc": desc,
        }

    ranges = sorted((by_name[n]["id_min"], by_name[n]["id_max"], n) for n in order)
    prev_max = None
    prev_name = None
    for id_min, id_max, name in ranges:
        if prev_max is not None and id_min <= prev_max:
            raise ValueError(
                f"overlapping group ranges: {prev_name} ends at 0x{prev_max:04X}, "
                f"but {name} starts at 0x{id_min:04X}"
            )
        prev_max = id_max
        prev_name = name

    return order, by_name

def main():
    if len(sys.argv) != 3:
        print("usage: gen_opcodes.py <opcodes.json> <out_header>")
        return 2
    spec_path = Path(sys.argv[1])
    out_header = Path(sys.argv[2])

    data = json.loads(spec_path.read_text(encoding="utf-8"))
    ns = data.get("namespace", "server::core::protocol")
    items = data.get("opcodes", [])

    group_order, groups = parse_groups(data)
    require_group = bool(group_order)
    group_index = {name: i for i, name in enumerate(group_order)}

    # 검증 및 정렬
    seen_ids = set()
    seen_names = set()
    parsed = []
    for it in items:
        name = str(it.get("name", "")).strip()
        if not name:
            raise ValueError("opcode.name is required")

        val = parse_id(it.get("id"))
        if not (0 <= val <= 0xFFFF):
            raise ValueError(f"opcode id out of range: {name}={val}")

        desc = str(it.get("desc", ""))
        group = str(it.get("group", "")).strip()
        direction = str(it.get("dir", "")).strip()

        if require_group:
            if not group:
                raise ValueError(f"missing group for opcode: {name}")
            if group not in groups:
                raise ValueError(f"unknown group '{group}' for opcode: {name}")
            g = groups[group]
            if not (g["id_min"] <= val <= g["id_max"]):
                raise ValueError(
                    f"opcode id out of group range: {name}=0x{val:04X} not in {group}"
                    f" [0x{g['id_min']:04X}..0x{g['id_max']:04X}]"
                )

        if name in seen_names:
            raise ValueError(f"중복 name: {name}")
        if val in seen_ids:
            raise ValueError(f"중복 id: 0x{val:04X}")
        seen_names.add(name)
        seen_ids.add(val)
        parsed.append((val, name, desc, group, direction))

    def fmt_desc(desc: str, direction: str) -> str:
        d = str(desc)
        if direction:
            return f"[{direction}] {d}" if d else f"[{direction}]"
        return d

    if require_group:
        parsed.sort(key=lambda x: (group_index[x[3]], x[0]))
        by_group = {g: [] for g in group_order}
        for val, name, desc, group, direction in parsed:
            by_group[group].append((val, name, desc, direction))

        lines = []
        for gname in group_order:
            g = groups[gname]
            header = f"// === {gname} (0x{g['id_min']:04X}..0x{g['id_max']:04X})"
            if g["desc"]:
                header += f": {g['desc']}"
            lines.append(header)
            for val, name, desc, direction in by_group[gname]:
                lines.append(LINE_TEMPLATE.format(name=name, value=val, desc=fmt_desc(desc, direction)))
            lines.append("")
    else:
        parsed.sort(key=lambda x: x[0])
        lines = [LINE_TEMPLATE.format(name=name, value=val, desc=fmt_desc(desc, direction)) for val, name, desc, _group, direction in parsed]

    # Keep opcode_name() stable and sorted by numeric id.
    cases_src = sorted(parsed, key=lambda x: x[0])
    case_lines = [f"    case 0x{val:04X}: return \"{name}\";" for val, name, _desc, _group, _direction in cases_src]
    cases = "\n".join(case_lines)
    body = "\n".join(lines) + "\n\n" + NAME_FUNC_TEMPLATE.format(cases=cases)
    text = HEADER_TEMPLATE.format(ns=ns, body=body)

    out_header.parent.mkdir(parents=True, exist_ok=True)
    out_header.write_text(text + "\n", encoding="utf-8")
    print(f"[gen_opcodes] generated {out_header}")

if __name__ == "__main__":
    sys.exit(main())
