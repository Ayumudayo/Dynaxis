# Dynaxis

C++20 서버 코어와 MSA 채팅 스택입니다. HAProxy → gateway → chat server로 연결하고 Redis로 노드 간 전달과 라우팅을, Postgres와 write-behind worker로 영속화를 처리합니다.

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build
```

기본 로컬 접속은 `127.0.0.1:6000`이며 개발 인증 모드입니다. 외부 연결에 사용할 signed 인증은 [인증 문서](docs/ops/authenticated-chat.md)를 따릅니다.

[문서](docs/README.md) · [구조](docs/architecture/core-and-msa.md) · [빌드](docs/build.md) · [테스트](docs/tests.md) · [개발 클라이언트](client_gui/README.md)
