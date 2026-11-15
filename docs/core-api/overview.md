# 공개 Core API

공개 헤더 목록은 `compatibility-matrix.json`과 `tools/check_core_api_contracts.py`의 Stable 목록으로 관리한다. public consumer는 Stable 헤더만 사용하고 앱의 내부 Session/adapter 경계는 별도로 검사한다.

주요 범위는 app lifecycle, TCP connection/listener, dispatcher, concurrent queue/scheduler, memory, metrics, protocol, discovery, storage execution이다. [4.0 migration](foundation-v4-migration.md), [최소 구성](../build.md).
