# 저장소 구조/분리 가이드

> 목적: 현재 폴더 구성의 미흡함을 개선하고, 각 프로젝트를 완전히 분리된 형태로 관리할 수 있도록 구조/규칙/이행 절차를 문서화한다. 장기적으로 MSA 전환에 무리 없이 확장 가능해야 한다. 또한, 확정되지 않은 프로젝트명(예: knights)의 네임스페이스/타깃/파일명 사용을 금지한다.

## 원칙
- 프로젝트 완전 분리: 각 서브프로젝트는 독립적으로 빌드/테스트/배포 가능해야 한다.
- 재사용성: `core`는 다른 서버/서비스에서도 재사용 가능한 범용 라이브러리여야 한다.
- 역할 기반 네이밍: 특정 프로젝트명(예: Knights/knights) 금지. 역할 중심 이름만 사용한다.
- 경계 명확화: 공용 인터페이스(헤더, gRPC, 바이너리 프로토콜) 외의 내부 구현 의존 금지.
- 점진적 이행: 큰 리네임/이동은 작은 커밋 단위로, 빌드가 항상 통과하도록 진행한다.

참고: `docs/naming-conventions.md`, `docs/msa-architecture.md`, `docs/core-design.md`

## 목표 디렉터리 구조(제안)
```
/                         # 최상위(메타/환경/빌드)
├─ CMakeLists.txt         # 상위 CMake: 하위 프로젝트 위임(add_subdirectory)
├─ docs/                  # 문서 일체 (본 문서 포함)
├─ tools/                 # 코드 생성/보조 스크립트
├─ scripts/               # 빌드/배포/개발 편의 스크립트
├─ protocol/              # 커스텀 바이너리 프로토콜 스키마/맵/생성 스크립트
├─ proto/                 # Protobuf/gRPC .proto 정의 및 생성 규칙
├─ core/
│  ├─ CMakeLists.txt
│  ├─ include/server/core/...   # 공개 헤더(패키지 공개 API)
│  └─ src/...                   # 구현
├─ services/               # MSA로의 확장 대비 서비스 단위 분리
│  ├─ gateway/             # (현 server 모놀리스를 여기로 전개 예정)
│  │  ├─ CMakeLists.txt
│  │  ├─ include/server/gateway/...  # 필요한 경우 공개 헤더
│  │  └─ src/...
│  ├─ auth/                # (후속) 인증 서비스
│  ├─ chat/                # (후속) 채팅 서비스
│  ├─ match/               # (후속) 매치메이킹 서비스
│  └─ world/               # (후속) 월드 서비스
├─ clients/
│  └─ devcli/              # 기존 devclient를 이동
│     ├─ CMakeLists.txt
│     └─ src/...
└─ third_party/            # (선택) 외부 종속 소스 동봉 시, 서브모듈/벤더링 위치
```

메모
- 루트 `include/`와 `src/`는 단계적으로 제거하고, 각 프로젝트 내부 `include/<패키지>/`, `src/`로 이동한다.
- 현재 `server/`는 `services/gateway/`로 이전(또는 단계적으로 동거 후 완전 이전)한다.
- `devclient/`는 `clients/devcli/`로 이동한다.
- `protocol/`(커스텀 바이너리)과 `proto/`(Protobuf)는 목적이 다르므로 유지하되, 생성물은 소스 트리에 커밋하지 않는다.

## 빌드 시스템(CMake) 규칙
- 상위 CMake는 전역 include 디렉터리/컴파일 옵션을 최소화하고, 하위 타깃에만 필요한 설정을 부여한다.
- 타깃/네임스페이스/출력물에 프로젝트명(예: knights) 금지. 예시:
  - 라이브러리: `server_core` (STATIC/SHARED 선택), `gateway_service_lib`
  - 실행 파일: `gateway_service`, `dev_chat_cli`
  - C++ 네임스페이스: `server::core`, `server::gateway`
- `core`는 PUBLIC 헤더만 노출: `target_include_directories(server_core PUBLIC include)`
- 종속 관계는 단방향 유지: `services/*` → `core`, `clients/*` → `core` (역참조 금지)
- 코드생성(예: opcode/wire codec, Protobuf)은 각각 전용 `add_custom_command/target`로 캡슐화하고, 소비 타깃에서
  `add_dependencies(… generate_xxx)`로 연결한다.
- 설치/배포(선택): `core`는 `install(TARGETS … EXPORT …)`로 외부 서버에서도 재사용 가능하게 패키징.
- 모듈 온/오프: `BUILD_SERVER_STACK`, `BUILD_GATEWAY_APP`, `BUILD_LOAD_BALANCER_APP`, `BUILD_DEVCLIENT_APP`, `BUILD_WRITE_BEHIND_TOOLS`, `BUILD_SERVER_TESTS` 옵션으로 필요한 서브프로젝트만 선택적으로 빌드한다. 테스트는 `BUILD_GTEST_TESTS`/`BUILD_CONTRACT_TESTS`로 세분화해 GTest 의존 경로와 비의존 경로를 분리한다.

## 네임스페이스/명명 규칙(요약)
- 금지: `knights`, `Knights`를 코드/네임스페이스/바이너리/타깃/패키지명에 사용.
- 허용 예시: `server::core`, `server::gateway`, `auth_service`, `match_service` 등 역할/도메인 기반 이름.
- 자세한 규칙은 `docs/naming-conventions.md`를 따른다.

## 이행 절차(권장 단계)
1) 가드레일 설정
   - CI에서 빌드/테스트 파이프라인 추가(있다면 강화). 전역 grep으로 `knights` 문자열 사용 금지 검사.
   - 루트에 `.cmake-format.yml`/`.clang-format` 등 코딩 규약(선택)을 명시해 일관성 확보.

2) 디렉터리 스켈레톤 마련(빌드 비깨짐 방지)
   - `services/gateway/`와 `clients/devcli/` 디렉터리만 먼저 생성하고, 상위 CMake에서 빈 타깃을 추가.
   - 기존 `server/`, `devclient/`는 그대로 유지한 채 두 구조가 공존하도록 한다.

3) `server/` → `services/gateway/` 점진 이전
   - 소스/헤더/리소스를 서비스 단위로 이동. 네임스페이스를 `server::gateway`로 정리.
   - 타깃명/출력물명은 `gateway_service`로 변경.
   - 전역 include 의존을 제거하고 `target_link_libraries(gateway_service PRIVATE server_core)`로 연결.

4) `devclient/` → `clients/devcli/` 이동
   - 타깃명 `dev_chat_cli`(또는 역할 반영)로 변경. `core`만 참조하도록 정리.

5) 루트 `include/`, `src/` 내용 정리
   - 각 패키지 내부 `include/<패키지>/`, `src/`로 이전. 외부가 필요 없는 헤더는 PRIVATE로 전환.

6) `core` 정비
   - 공개 API만 `core/include/server/core/`로 노출. 모듈 경계를 흐리는 유틸/전역 단일체 제거.
   - 외부 의존 최소화(Boost, fmt 등). 선택적 기능은 `option()`으로 분리.

7) 프로토콜/코드생성 체계화
   - `protocol/`(바이너리) → 헤더 생성 타깃, `proto/`(gRPC) → 언어별 생성 타깃을 분리 유지.
   - 생성 산출물은 `build/` 하위에만 위치(소스 커밋 금지).

8) 청소/검증
   - 사용처 업데이트/불필요 파일 제거. CMake 캐시/빌드 재생성 후 전체 빌드 검증.
   - 문서 갱신: 본 문서, `build.md`, `server-architecture.md` 반영.

## 점검 체크리스트
- [ ] `grep -R "knights"` 결과 없음(코드/빌드/문서/스크립트 포함)
- [ ] 각 서브프로젝트 단독 빌드 성공(`core`, `services/gateway`, `clients/devcli`)
- [ ] `core`의 public 헤더 외 내부 구현 노출 없음
- [ ] `services/*` → `core` 단방향 의존 보장(역의존/순환 없음)
- [ ] 코드생성 타깃 분리 및 소비 타깃과 의존 관계 명시
- [ ] 루트 전역 include 제거, 패키지 로컬 include로 일관화
- [ ] CI 파이프라인에서 전체/부분 빌드 매트릭스 통과

## 부록: 기존 트리에서의 매핑(초안)
- `server/` → `services/gateway/`
- `devclient/` → `clients/devcli/`
- `include/` → 각 패키지의 `include/<패키지>/`
- `src/` → 각 패키지의 `src/`
- `protocol/`, `proto/` → 유지(생성 출력물은 소스 커밋 금지)

## 다음 단계
- 상위 CMake에 `services/gateway`와 `clients/devcli`의 스텁 타깃을 추가하고, 이동을 시작한다.
- 명명 규칙과 MSA 설계를 교차 검토하여 서비스 경계를 구체화한다.
- `core` API를 최소-충분 집합으로 다듬고 단위 테스트를 강화한다.
