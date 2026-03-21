#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import json
import sys
from pathlib import Path
from typing import Any

HEADER_TEMPLATE = """// 자동 생성 파일: tools/gen_opcodes.py에 의해 생성됨
/**
 * @file
 * @brief opcode 상수와 transport/runtime 정책 표를 같은 소스에서 자동 생성한 헤더입니다.
 *
 * 상수 선언과 정책 표가 손으로 따로 관리되면 "ID는 바뀌었는데 정책은 예전 값" 같은 드리프트가 생기기 쉽습니다.
 * 이 헤더는 JSON spec 한 곳에서 두 정보를 함께 생성해, 런타임과 문서가 같은 계약을 보도록 만듭니다.
 *
 * @note tools/gen_opcodes.py와 JSON spec에서 다시 생성됩니다. 직접 수정하면 다음 생성 때 덮어써집니다.
 */
#pragma once
#include <cstdint>
#include <string_view>
#include "server/core/protocol/opcode_policy.hpp"

namespace {ns} {{
{body}
}} // namespace {ns}
"""

LINE_TEMPLATE = "static constexpr std::uint16_t {name:<24} = 0x{value:04X}; // {desc}"

NAME_FUNC_TEMPLATE = """

/**
 * @brief opcode ID를 사람이 읽을 수 있는 기호 이름으로 변환합니다.
 *
 * 로그, 메트릭, 디버거에서는 숫자보다 `MSG_*` 이름이 훨씬 읽기 쉽습니다. 이 표를 generated header에 함께 두면
 * 런타임 코드가 별도 이름 매핑 테이블을 중복 유지하지 않아도 됩니다.
 *
 * @param id 조회할 opcode ID
 * @return 매칭된 opcode 이름, 미정의 ID면 빈 문자열
 */
inline constexpr std::string_view opcode_name( std::uint16_t id ) noexcept
{{
  switch( id )
  {{
{cases}
    default: return std::string_view{{}};
  }}
}}
""".lstrip("\n")

POLICY_FUNC_TEMPLATE = """

/**
 * @brief opcode ID에 대한 런타임 정책 메타데이터를 반환합니다.
 *
 * 이 함수는 "어느 세션 상태에서 허용되는가", "어느 실행기에서 처리되는가", "어느 transport/delivery를 기대하는가"를
 * 한 표로 고정합니다. 정책 표를 코드 여러 곳에 흩어 놓으면 새로운 opcode 추가 때 누락이 생기기 쉽습니다.
 *
 * @param id 조회할 opcode ID
 * @return 매칭된 opcode 정책, 미정의 ID면 기본 정책
 */
inline constexpr server::core::protocol::OpcodePolicy opcode_policy( std::uint16_t id ) noexcept
{{
  switch( id )
  {{
{cases}
    default: return server::core::protocol::default_opcode_policy();
  }}
}}
""".lstrip("\n")

SESSION_STATUS_MAP = {
    "any": "server::core::protocol::SessionStatus::kAny",
    "authenticated": "server::core::protocol::SessionStatus::kAuthenticated",
    "in_room": "server::core::protocol::SessionStatus::kInRoom",
    "admin": "server::core::protocol::SessionStatus::kAdmin",
}

PROCESSING_PLACE_MAP = {
    "inline": "server::core::protocol::ProcessingPlace::kInline",
    "worker": "server::core::protocol::ProcessingPlace::kWorker",
    "room_strand": "server::core::protocol::ProcessingPlace::kRoomStrand",
}

TRANSPORT_MASK_MAP = {
    "none": "server::core::protocol::TransportMask::kNone",
    "tcp": "server::core::protocol::TransportMask::kTcp",
    "udp": "server::core::protocol::TransportMask::kUdp",
    "both": "server::core::protocol::TransportMask::kBoth",
}

DELIVERY_CLASS_MAP = {
    "reliable_ordered": "server::core::protocol::DeliveryClass::kReliableOrdered",
    "reliable": "server::core::protocol::DeliveryClass::kReliable",
    "unreliable_sequenced": "server::core::protocol::DeliveryClass::kUnreliableSequenced",
}


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
            raise ValueError("group.name 필드는 필수입니다")
        if name in by_name:
            raise ValueError(f"중복 그룹: {name}")

        id_min = parse_id(g.get("id_min"))
        id_max = parse_id(g.get("id_max"))
        if not (0 <= id_min <= 0xFFFF and 0 <= id_max <= 0xFFFF and id_min <= id_max):
            raise ValueError(
                f"유효하지 않은 그룹 범위: {name} [0x{id_min:04X}..0x{id_max:04X}]"
            )

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
                f"그룹 범위가 겹칩니다: {prev_name}의 끝은 0x{prev_max:04X}, "
                f"{name}의 시작은 0x{id_min:04X}"
            )
        prev_max = id_max
        prev_name = name

    return order, by_name


def parse_policy_token(
    raw: Any,
    default_value: str,
    mapping: dict[str, str],
    field_name: str,
    opcode_name: str,
) -> str:
    token = str(default_value if raw is None else raw).strip().lower()
    if token not in mapping:
        valid = ", ".join(sorted(mapping.keys()))
        raise ValueError(
            f"{opcode_name}의 {field_name} 값이 유효하지 않습니다: '{token}' (허용: {valid})"
        )
    return mapping[token]


def main():
    if len(sys.argv) < 3:
        print(
            "사용법: gen_opcodes.py <opcodes.json> <out_header> [--group <name>] [--namespace <ns>]"
        )
        return 2

    spec_path = Path(sys.argv[1])
    out_header = Path(sys.argv[2])

    selected_group = None
    namespace_override = None
    i = 3
    while i < len(sys.argv):
        arg = sys.argv[i]
        if arg == "--group":
            if i + 1 >= len(sys.argv):
                raise ValueError("--group requires a value")
            selected_group = sys.argv[i + 1]
            i += 2
            continue
        if arg == "--namespace":
            if i + 1 >= len(sys.argv):
                raise ValueError("--namespace requires a value")
            namespace_override = sys.argv[i + 1]
            i += 2
            continue
        raise ValueError(f"알 수 없는 옵션: {arg}")

    data = json.loads(spec_path.read_text(encoding="utf-8"))
    ns = namespace_override or data.get("namespace", "server::core::protocol")
    items = data.get("opcodes", [])

    group_order, groups = parse_groups(data)
    require_group = bool(group_order)
    if selected_group is not None:
        if not require_group:
            raise ValueError(
                "--group 옵션은 groups 섹션이 있는 spec에서만 사용할 수 있습니다"
            )
        if selected_group not in groups:
            raise ValueError(f"알 수 없는 group '{selected_group}'")
        group_order = [selected_group]
    group_index = {name: i for i, name in enumerate(group_order)}

    # 검증 및 정렬
    seen_ids = set()
    seen_names = set()
    parsed = []
    for it in items:
        name = str(it.get("name", "")).strip()
        if not name:
            raise ValueError("opcode.name 필드는 필수입니다")

        val = parse_id(it.get("id"))
        if not (0 <= val <= 0xFFFF):
            raise ValueError(f"opcode id 범위를 벗어났습니다: {name}={val}")

        desc = str(it.get("desc", ""))
        group = str(it.get("group", "")).strip()
        direction = str(it.get("dir", "")).strip()

        if require_group:
            if not group:
                raise ValueError(f"opcode의 group이 누락되었습니다: {name}")
            if group not in groups:
                raise ValueError(f"알 수 없는 group '{group}': opcode={name}")
            g = groups[group]
            if not (g["id_min"] <= val <= g["id_max"]):
                raise ValueError(
                    f"opcode id가 group 범위를 벗어났습니다: {name}=0x{val:04X}, group={group}"
                    f" [0x{g['id_min']:04X}..0x{g['id_max']:04X}]"
                )
            if selected_group is not None and group != selected_group:
                continue

        required_state = parse_policy_token(
            it.get("required_state"), "any", SESSION_STATUS_MAP, "required_state", name
        )
        processing_place = parse_policy_token(
            it.get("processing_place"),
            "inline",
            PROCESSING_PLACE_MAP,
            "processing_place",
            name,
        )
        transport = parse_policy_token(
            it.get("transport"), "tcp", TRANSPORT_MASK_MAP, "transport", name
        )
        delivery = parse_policy_token(
            it.get("delivery"), "reliable_ordered", DELIVERY_CLASS_MAP, "delivery", name
        )

        channel = parse_id(it.get("channel", 0))
        if not (0 <= channel <= 0xFF):
            raise ValueError(f"channel 범위를 벗어났습니다[0..255]: {name}={channel}")

        if name in seen_names:
            raise ValueError(f"중복 name: {name}")
        if val in seen_ids:
            raise ValueError(f"중복 id: 0x{val:04X}")
        seen_names.add(name)
        seen_ids.add(val)
        parsed.append(
            (
                val,
                name,
                desc,
                group,
                direction,
                required_state,
                processing_place,
                transport,
                delivery,
                channel,
            )
        )

    def fmt_desc(desc: str, direction: str) -> str:
        d = str(desc)
        if direction:
            return f"[{direction}] {d}" if d else f"[{direction}]"
        return d

    if require_group:
        parsed.sort(key=lambda x: (group_index[x[3]], x[0]))
        by_group = {g: [] for g in group_order}
        for (
            val,
            name,
            desc,
            group,
            direction,
            _required_state,
            _processing_place,
            _transport,
            _delivery,
            _channel,
        ) in parsed:
            by_group[group].append((val, name, desc, direction))

        lines = []
        for gname in group_order:
            g = groups[gname]
            header = f"// === {gname} (0x{g['id_min']:04X}..0x{g['id_max']:04X})"
            if g["desc"]:
                header += f": {g['desc']}"
            lines.append(header)
            for val, name, desc, direction in by_group[gname]:
                lines.append(
                    LINE_TEMPLATE.format(
                        name=name, value=val, desc=fmt_desc(desc, direction)
                    )
                )
            lines.append("")
    else:
        parsed.sort(key=lambda x: x[0])
        lines = [
            LINE_TEMPLATE.format(name=name, value=val, desc=fmt_desc(desc, direction))
            for val, name, desc, _group, direction, _required_state, _processing_place, _transport, _delivery, _channel in parsed
        ]

    # opcode_name()/opcode_policy()는 숫자 ID 기준으로 안정적으로 정렬해 diff와 코드 리뷰를 읽기 쉽게 유지합니다.
    cases_src = sorted(parsed, key=lambda x: x[0])
    case_lines = [
        f'    case 0x{val:04X}: return "{name}";'
        for val, name, _desc, _group, _direction, _required_state, _processing_place, _transport, _delivery, _channel in cases_src
    ]
    cases = "\n".join(case_lines)

    policy_case_lines = [
        (
            f"    case 0x{val:04X}: return server::core::protocol::OpcodePolicy"
            f"{{{required_state}, {processing_place}, {transport}, {delivery}, {channel}}};"
        )
        for val, _name, _desc, _group, _direction, required_state, processing_place, transport, delivery, channel in cases_src
    ]
    policy_cases = "\n".join(policy_case_lines)

    body = (
        "\n".join(lines)
        + "\n\n"
        + NAME_FUNC_TEMPLATE.format(cases=cases)
        + "\n"
        + POLICY_FUNC_TEMPLATE.format(cases=policy_cases)
    )
    text = HEADER_TEMPLATE.format(ns=ns, body=body)

    out_header.parent.mkdir(parents=True, exist_ok=True)
    out_header.write_text(text + "\n", encoding="utf-8")
    print(f"[gen_opcodes] generated {out_header}")


if __name__ == "__main__":
    sys.exit(main())
