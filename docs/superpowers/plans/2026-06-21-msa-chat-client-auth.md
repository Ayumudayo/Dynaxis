# MSA Chat Client Auth Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a minimal auth-backed MSA chat stack where anonymous users receive real service-issued identities and the ImGui client participates in the same auth/session flow.

**Architecture:** Add a small `auth_app` service that owns anonymous/registered principals and auth sessions. Keep `gateway_app` as the only public TCP ingress: it resolves login requests through `auth_app`, forwards a short-lived signed internal auth context to `server_app`, and `server_app` consumes that context instead of trusting client-supplied usernames. `client_gui` continues to talk only to the gateway, uses anonymous/token login modes through the existing login packet, and preserves both new auth identity fields and existing server continuity fields.

**Tech Stack:** C++20, Boost.Asio, protobuf, existing packet/opcode wire format, Postgres/libpqxx, Redis Streams/PubSub, Docker Compose, GTest, Python stack verification scripts, Dear ImGui/GLFW client.

---

## Scope Check

This is intentionally a vertical slice across several services. Keep the first deliverable narrow: anonymous auth principal/session -> gateway verification -> signed gateway-to-server login context -> server login -> client state update. Do not migrate FPS/world runtime, plugin/Lua, write-behind semantics, admin defaults, or full topology ownership in this tranche.

The work is split so each task leaves the stack buildable and testable. If execution time becomes an issue, stop after Task 6; that yields the minimum MSA-authenticated chat path with client support.

Tasks 7-9 are follow-up hardening work. They are included so the direction is explicit, but they should not be bundled into the minimum slice unless the user explicitly broadens the delivery.

## Identity And Token Semantics

Freeze these rules before implementation starts:

- `display_name`: user-facing label only. It is not an authorization key and may collide.
- `principal_id`: stable auth identity issued by `auth_app`; use it for ownership, moderation, audit correlation, and admin authorization.
- `auth_session_id`: auth service session id issued by `auth_app`; use it for auth/session tracing and gateway routing, not as a replacement for server continuity ids.
- `logical_session_id`: existing `server_app` continuity/session field in `LoginRes`; do not repurpose it as `auth_session_id`.
- `resume_token`: existing server continuity token. It remains distinct from auth access tokens and gateway internal context tokens.
- client access token: optional client -> gateway credential. It may be empty for anonymous login and must never be accepted as a gateway-to-server internal token.
- gateway internal auth context: gateway -> server only, short-lived, HMAC signed, audience-bound, replay-protected, and never stored by the client.

The first tranche may expose `principal_id`, `auth_session_id`, `auth_class`, and `role` in `LoginRes` using new backward-compatible fields. Existing fields keep their current meaning.

## File Structure

### New Auth Contract And Service

- Create: `auth/include/auth/session_contract.hpp`
  - Defines auth subject/session types shared by `auth_app`, gateway adapter tests, and server internal auth parsing.
- Create: `auth/src/session_contract.cpp`
  - Validation and serialization helpers for signed internal auth context.
- Create: `auth/src/auth_app.cpp`
  - Minimal HTTP service exposing `/healthz`, `/readyz`, and `POST /v1/session/resolve`.
- Create: `auth/CMakeLists.txt`
  - Builds `auth_contract` static library and `auth_app` executable.
- Modify: `CMakeLists.txt`
  - Add `BUILD_AUTH_APP` option and `add_subdirectory(auth)`.
- Modify: `Dockerfile`
  - Build and copy `auth_app`; add `auth-runtime` stage.
- Modify: `docker/stack/docker-compose.yml`
  - Add `auth-app` service and wire gateway env.

### Protocol And Client Contract

- Modify: `proto/auth.proto`
  - Optional in the first slice. Either add a real `auth_proto` build target first, or leave this proto untouched and make the first auth boundary HTTP+JSON via `auth/session_contract.hpp`.
- Modify: `proto/wire.proto`
  - Add backward-compatible client-visible auth fields if the client must display them: `principal_id`, `auth_session_id`, `auth_class`, `role`. Do not reuse `logical_session_id` or `resume_token` for auth identity.
- Modify: `proto/CMakeLists.txt`
  - Only needed if `proto/auth.proto` becomes part of tranche 1. Current build graph only generates `wire_proto`, so add `auth_proto` deliberately or defer proto auth changes.
- Modify: `server/protocol/game_opcodes.json`
  - Add a versioned login request opcode only if the legacy `MSG_LOGIN_REQ` payload cannot carry a gateway-only internal envelope safely. Prefer a backward-compatible envelope first, but freeze the envelope before gateway/server edits.
- Modify: `tools/gen_opcode_docs.py` outputs via normal generation command when opcodes change.

### Gateway

- Create: `gateway/include/gateway/auth/auth_service_client.hpp`
  - Defines `AuthServiceClient` interface and result structs.
- Create: `gateway/src/auth_service_client.cpp`
  - Minimal HTTP client for `POST /v1/session/resolve`.
- Modify: `gateway/include/gateway/auth/authenticator.hpp`
  - Mark `NoopAuthenticator` as dev-only in comments; add fail-closed default path.
- Modify: `gateway/src/gateway_app.cpp`
  - Do not default production gateway to accepting all auth.
- Modify: `gateway/src/gateway_configuration.cpp`
  - Parse `AUTH_SERVICE_URL`, `AUTH_REQUIRED`, `GATEWAY_TO_SERVER_SIGNING_SECRET`, and production-safe anonymous mode.
- Modify: `gateway/src/gateway_connection.cpp`
  - Resolve login through auth service, derive stable routing key from `principal_id` or `auth_session_id`, and forward signed auth context to backend.
- Modify: `gateway/src/gateway_metrics_text.cpp`
  - Add auth success/failure/fail-open/fail-closed counters.

### Server Chat Runtime

- Create: `server/src/chat/auth_context.hpp`
  - Parses and validates internal gateway auth context.
- Create: `server/src/chat/auth_context.cpp`
  - HMAC verification, expiry, and field normalization.
- Modify: `server/src/chat/handlers_login.cpp`
  - Accept internal auth context and stop creating users/sessions from raw client username on the new path.
- Modify: `server/src/chat/chat_service_state.hpp`
  - Track `principal_id`, `auth_session_id`, `auth_class`, `role`, and display name separately.
- Modify: `server/src/chat/chat_command_authorization.cpp`
  - Authorize by role/auth class, not by display name string.
- Modify: `server/src/chat/handlers_chat.cpp`
  - Decide anonymous chat policy explicitly from `auth_class`.
- Modify: `server/src/chat/chat_message_delivery.cpp`
  - Carry display name snapshots while preserving stable sender identity internally.
- Modify: `server/src/app/config.cpp`
  - Parse `GATEWAY_TO_SERVER_SIGNING_SECRET`, `SERVER_REQUIRE_TRUSTED_AUTH_CONTEXT`.

### Data Ownership And Migrations

- Create: `tools/migrations/0002_auth_schema.sql`
  - Creates `auth.principals`, `auth.sessions`, indexes, and minimal grants placeholders.
- Create: `tools/migrations/0003_chat_identity_columns.sql`
  - Adds nullable `principal_id`, `auth_session_id`, and `sender_display_name` columns needed for migration.
- Create: `tools/migrations/0004_outbox_inbox_minimal.sql`
  - Adds `chat.outbox_events`, `auth.outbox_events`, and `audit.session_events` shape if needed.
- Modify: `tools/migrations/runner.cpp`
  - Add a way to exclude dev seed migrations from production mode.
- Modify: `tools/migrations/0100_seed_dev.sql`
  - Keep dev seed explicitly dev-only.

### Client GUI

- Modify: `client_gui/include/client/client_app.hpp`
  - Add `AuthState`, `SessionState`, and explicit connection/login phases.
- Modify: `client_gui/include/client/net_client.hpp`
  - Expand `OnLoginRes`; preserve existing token transport; add send/receive safety limits.
- Modify: `client_gui/src/client_app.cpp`
  - Store token/session/resume fields, guard command sending by login state, and support anonymous session mode.
- Modify: `client_gui/src/net_client.cpp`
  - Preserve `LoginRes` fields, enforce max receive body, enforce send queue budget, and reject any attempt to send gateway-internal auth context from the client.
- Modify: `client_gui/include/client/gui_manager.hpp`
  - Add token field, anonymous mode toggle, and saved session UI state.
- Modify: `client_gui/src/gui_manager.cpp`
  - Show anonymous/token login mode, connection state, auth class, session id, and resume status.
- Modify: `client_gui/README.md`
  - Document anonymous auth, token login, and current developer-client limitations.

### Tests And Verification

- Create: `tests/auth/CMakeLists.txt`
- Create: `tests/auth/test_auth_session_contract.cpp`
- Create: `tests/gateway/test_auth_service_client.cpp`
- Modify: `tests/gateway/CMakeLists.txt`
- Modify: `tests/server/test_server_chat.cpp`
- Create: `tests/client/test_net_client_auth_contract.cpp` if client tests are enabled in the chosen preset; otherwise create `tests/python/test_client_wire_contracts.py`.
- Modify: `tests/python/verify_chat.py`
  - Add auth-aware login path.
- Create: `tests/python/verify_anonymous_auth_chat.py`
  - End-to-end stack check for anonymous auth.
- Modify: `.github/workflows/ci-stack.yml`
  - Include topology generator/auth/client contract changes in path filters.

---

## Task 0: Freeze Identity Mapping And Build Scaffolding

**Files:**
- Modify: `docs/ops/authenticated-chat-stack.md` or create a short draft in this plan until final docs task.
- Modify: `CMakeLists.txt`
- Create: `auth/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Create: `tests/auth/CMakeLists.txt`
- Optional: `proto/CMakeLists.txt` only if `auth_proto` is required in tranche 1.

- [ ] **Step 1: Write the identity mapping table**

Document the exact mapping before code edits:

| Concept | Owner | Wire exposure | Persistence | Notes |
|---|---|---|---|---|
| `display_name` | `auth_app` suggests, `server_app` snapshots | `LoginRes.effective_user`, chat broadcasts | chat display snapshots | never authorize by this |
| `principal_id` | `auth_app` | new `LoginRes.principal_id` if client-visible | auth/users/chat ownership/audit | stable identity |
| `auth_session_id` | `auth_app` | new `LoginRes.auth_session_id` if client-visible | auth sessions/gateway routing | not `logical_session_id` |
| `logical_session_id` | `server_app` | existing `LoginRes.logical_session_id` | server continuity | keep existing contract |
| `resume_token` | `server_app` | existing `LoginRes.resume_token` | server continuity | not an auth access token |

- [ ] **Step 2: Choose tranche-1 auth contract transport**

Default decision: use HTTP+JSON for `auth_app` -> gateway in tranche 1 and keep `proto/auth.proto` deferred unless a real `auth_proto` target is added first.

If protobuf is used, add `auth_proto` to `proto/CMakeLists.txt` and prove it is linked by at least one test target. Do not edit `proto/auth.proto` as a dead spec.

- [ ] **Step 3: Add empty build/test scaffolding**

Add:

- `BUILD_AUTH_APP` option.
- `auth/` subdirectory with a placeholder `auth_contract` target.
- `tests/auth` registration in `tests/CMakeLists.txt`.

This keeps Task 1 focused on contract behavior instead of fighting missing CMake plumbing.

- [ ] **Step 4: Define gateway-to-server compatibility envelope**

Freeze one of these before implementation:

- Preferred: legacy `MSG_LOGIN_REQ` with token field containing a gateway-only envelope prefix, for example `gateway-auth-v1:<signed_context>`, and the envelope includes any original client token/resume token as a separate field.
- Alternative: new versioned internal login opcode used only from gateway to server.

Required envelope claims:

- `iss=gateway`
- `aud=server_app`
- `iat_unix_ms`
- `not_after_unix_ms`
- `jti`
- `kid`
- `principal_id`
- `auth_session_id`
- `display_name`
- `auth_class`
- `role`
- optional original client credential/resume token, never logged

The server must reject unsigned, expired, wrong-audience, replayed, and client-supplied internal envelopes.

- [ ] **Step 5: Commit scaffolding**

```powershell
git add CMakeLists.txt auth tests docs
git commit -m "chore: scaffold auth service contract"
```

---

## Task 1: Freeze The Target Contract

**Files:**
- Modify: `proto/wire.proto`
- Create: `auth/include/auth/session_contract.hpp`
- Create: `auth/src/session_contract.cpp`
- Test: `tests/auth/test_auth_session_contract.cpp`

- [ ] **Step 1: Write the failing auth contract tests**

Create `tests/auth/test_auth_session_contract.cpp` with tests for:

```cpp
TEST(AuthSessionContractTest, AnonymousResolveProducesStableRequiredFields) {
    auth::SessionResolveResult result{};
    result.principal_id = "00000000-0000-0000-0000-000000000001";
    result.auth_session_id = "00000000-0000-0000-0000-000000000002";
    result.display_name = "guest-1234";
    result.auth_class = auth::AuthClass::kAnonymous;
    result.role = auth::Role::kUser;
    EXPECT_TRUE(auth::is_valid_resolve_result(result));
}

TEST(AuthSessionContractTest, SignedContextRoundTripsAndRejectsTampering) {
    auth::TrustedAuthContext ctx{};
    ctx.issuer = "gateway";
    ctx.audience = "server_app";
    ctx.principal_id = "00000000-0000-0000-0000-000000000001";
    ctx.auth_session_id = "00000000-0000-0000-0000-000000000002";
    ctx.display_name = "guest-1234";
    ctx.auth_class = auth::AuthClass::kAnonymous;
    ctx.role = auth::Role::kUser;
    ctx.issued_at_unix_ms = 1700000000000ULL;
    ctx.not_after_unix_ms = 1700000030000ULL;
    ctx.key_id = "test-key";
    ctx.jti = "nonce-1";
    const auto signed_token = auth::sign_trusted_context(ctx, "test-secret");
    auth::ReplayCache replay_cache;
    EXPECT_TRUE(auth::verify_trusted_context(signed_token, "test-secret", "server_app", 1700000001000ULL, replay_cache).has_value());
    EXPECT_FALSE(auth::verify_trusted_context(signed_token, "test-secret", "server_app", 1700000002000ULL, replay_cache).has_value());
    EXPECT_FALSE(auth::verify_trusted_context(signed_token + "x", "test-secret", "server_app", 1700000002000ULL, replay_cache).has_value());
}
```

Also add negative cases for wrong audience, expired token, missing `jti`, default/empty secret, and unknown `kid`.

- [ ] **Step 2: Wire test target and verify it fails**

Run:

```powershell
pwsh scripts/build.ps1 -Config Debug -Target auth_session_contract_tests
```

Expected: build fails because `auth::SessionResolveResult`, `sign_trusted_context`, and `verify_trusted_context` do not exist.

- [ ] **Step 3: Define the minimal auth types**

In `auth/include/auth/session_contract.hpp`, add:

```cpp
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace auth {

enum class AuthClass { kAnonymous, kRegistered };
enum class Role { kUser, kModerator, kAdmin };

struct SessionResolveRequest {
    std::string login_name;
    std::string token;
    std::string remote_ip;
    bool allow_anonymous{true};
};

struct SessionResolveResult {
    std::string principal_id;
    std::string auth_session_id;
    std::string display_name;
    AuthClass auth_class{AuthClass::kAnonymous};
    Role role{Role::kUser};
    std::uint64_t expires_unix_ms{0};
    std::string access_token;
};

struct TrustedAuthContext {
    std::string issuer;
    std::string audience;
    std::string principal_id;
    std::string auth_session_id;
    std::string display_name;
    AuthClass auth_class{AuthClass::kAnonymous};
    Role role{Role::kUser};
    std::uint64_t issued_at_unix_ms{0};
    std::uint64_t not_after_unix_ms{0};
    std::string key_id;
    std::string jti;
};

class ReplayCache {
public:
    bool mark_seen(std::string_view jti, std::uint64_t expires_unix_ms);
};

bool is_valid_resolve_result(const SessionResolveResult& result);
std::string sign_trusted_context(const TrustedAuthContext& context, std::string_view secret);
std::optional<TrustedAuthContext> verify_trusted_context(
    std::string_view token,
    std::string_view secret,
    std::string_view expected_audience,
    std::uint64_t now_unix_ms,
    ReplayCache& replay_cache);

} // namespace auth
```

- [ ] **Step 4: Implement canonical JSON + HMAC signing**

Use existing OpenSSL availability and `server::core::security` patterns from `core/src/security/admin_command_auth.cpp`. Keep the format internal and explicit:

```text
kid + "." + base64url(canonical_json_payload) + "." + hex_hmac_sha256(kid + "." + payload, secret)
```

Do not introduce JWT libraries. This is internal gateway-to-server context, not public OAuth. Validation must check signature, `aud`, `iat`, `not_after`, `jti` replay, `kid`, and minimum secret quality.

- [ ] **Step 5: Update client wire contract**

Do not change `proto/auth.proto` unless Task 0 added a real `auth_proto` target. For the first slice, make `auth_app` HTTP+JSON and keep the C++ contract in `auth/session_contract.hpp`.

Preserve backward compatibility in `proto/wire.proto` by keeping existing `LoginRes` fields unchanged:

- `effective_user`: display name
- `logical_session_id`: existing server continuity id
- `resume_token`: existing server reconnect token
- `resume_expires_unix_ms`: reconnect expiry
- `world_id`: routing/world hint

Add new optional fields for auth identity if the client needs to display/persist them:

- `principal_id`
- `auth_session_id`
- `auth_class`
- `role`

- [ ] **Step 6: Run contract tests**

Run:

```powershell
pwsh scripts/build.ps1 -Config Debug -Target auth_session_contract_tests
ctest --test-dir build-windows -C Debug -R "AuthSessionContract" --output-on-failure
```

Expected: all `AuthSessionContract` tests pass.

- [ ] **Step 7: Commit**

```powershell
git add proto/wire.proto auth tests
git commit -m "feat: define auth session contract"
```

---

## Task 2: Add Minimal `auth_app`

**Files:**
- Create: `auth/src/auth_app.cpp`
- Create: `auth/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `Dockerfile`
- Modify: `docker/stack/docker-compose.yml`
- Test: `tests/python/verify_anonymous_auth_chat.py`

- [ ] **Step 1: Write a failing smoke test for auth resolve**

Add the first half of `tests/python/verify_anonymous_auth_chat.py`:

```python
def test_auth_resolve_anonymous(auth_base_url):
    response = requests.post(
        auth_base_url + "/v1/session/resolve",
        json={"login_name": "guest", "token": "", "allow_anonymous": True},
        timeout=5,
    )
    assert response.status_code == 200
    data = response.json()
    assert data["principal_id"]
    assert data["auth_session_id"]
    assert data["display_name"]
    assert data["auth_class"] == "anonymous"
    assert data["role"] == "user"
```

If the repo avoids `pytest`, implement it as a standalone script with `main()` and explicit `sys.exit(1)` on failure, matching existing `tests/python/verify_*.py` style.

- [ ] **Step 2: Verify the smoke test fails**

Run:

```powershell
python tests/python/verify_anonymous_auth_chat.py --auth-url http://127.0.0.1:39100
```

Expected: connection refused or HTTP 404 because `auth_app` does not exist.

- [ ] **Step 3: Implement `auth_app`**

Use `server::core::metrics::MetricsHttpServer` only if it supports JSON POST cleanly; otherwise implement the smallest Boost.Asio HTTP endpoint similar to existing admin/metrics style.

Required endpoints:

- `GET /healthz` -> `200 ok`
- `GET /readyz` -> `200 ready` when secret/config loaded
- `POST /v1/session/resolve` -> JSON result

For the first implementation, persist anonymous sessions in memory and return UUIDs. Do not add passwords or registration yet.

- [ ] **Step 4: Add build wiring**

In `CMakeLists.txt`:

```cmake
option(BUILD_AUTH_APP "Build auth_app" ON)
if (BUILD_AUTH_APP)
  add_subdirectory(auth)
endif()
```

In `auth/CMakeLists.txt`, build:

- `auth_contract`
- `auth_app`

- [ ] **Step 5: Add Docker runtime**

In `Dockerfile`, copy `/opt/dynaxis/bin/auth_app` and add:

```dockerfile
FROM runtime-base AS auth-runtime
COPY --from=builder /opt/dynaxis/bin/auth_app /app/auth_app
ENTRYPOINT ["/app/entrypoint.sh"]
CMD ["auth"]
```

Update `entrypoint.sh` only if it lacks `auth`.

- [ ] **Step 6: Add compose service**

In `docker/stack/docker-compose.yml`, add `auth-app` with:

- `AUTH_TO_GATEWAY_SIGNING_SECRET` if auth responses are signed in tranche 1
- `METRICS_PORT`
- no public host port by default except a dev override if needed
- healthcheck on `/readyz`

`/readyz` must fail when required secrets are empty, defaulted, or too short. Compose may use clearly named dev-only secrets, but production mode must reject them.

- [ ] **Step 7: Run verification**

Run:

```powershell
pwsh scripts/build.ps1 -Config Debug -Target auth_app
python tests/python/verify_anonymous_auth_chat.py --auth-url http://127.0.0.1:39100
```

Expected: build succeeds; smoke passes when `auth_app` is running locally or through an explicit dev override that publishes the auth port. The compose default must keep `auth-app` private.

- [ ] **Step 8: Commit**

```powershell
git add auth CMakeLists.txt Dockerfile docker/stack/docker-compose.yml tests/python/verify_anonymous_auth_chat.py
git commit -m "feat: add minimal auth service"
```

---

## Task 3: Gateway Auth Resolution

**Files:**
- Create: `gateway/include/gateway/auth/auth_service_client.hpp`
- Create: `gateway/src/auth_service_client.cpp`
- Modify: `gateway/CMakeLists.txt`
- Modify: `gateway/src/gateway_configuration.cpp`
- Modify: `gateway/src/gateway_connection.cpp`
- Modify: `gateway/src/gateway_app.cpp`
- Modify: `gateway/src/gateway_metrics_text.cpp`
- Test: `tests/gateway/test_auth_service_client.cpp`

- [ ] **Step 1: Write failing gateway auth client tests**

Create `tests/gateway/test_auth_service_client.cpp` with a fake HTTP response parser test:

```cpp
TEST(AuthServiceClientTest, ParsesResolveSessionResponse) {
    const std::string body = R"json({
      "principal_id":"00000000-0000-0000-0000-000000000001",
      "auth_session_id":"00000000-0000-0000-0000-000000000002",
      "display_name":"guest-1234",
      "auth_class":"anonymous",
      "role":"user",
      "expires_unix_ms":4102444800000
    })json";
    auto parsed = gateway::auth::parse_resolve_response(body);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->display_name, "guest-1234");
    EXPECT_EQ(parsed->routing_key(), "00000000-0000-0000-0000-000000000001");
}
```

- [ ] **Step 2: Verify the test fails**

Run:

```powershell
pwsh scripts/build.ps1 -Config Debug -Target gateway_auth_service_client_tests
```

Expected: target or parser does not exist.

- [ ] **Step 3: Implement auth service client interface**

`gateway/include/gateway/auth/auth_service_client.hpp`:

```cpp
namespace gateway::auth {

struct ResolveSessionRequest {
    std::string login_name;
    std::string token;
    std::string remote_ip;
    bool allow_anonymous{true};
};

struct ResolveSessionResponse {
    std::string principal_id;
    std::string auth_session_id;
    std::string display_name;
    std::string auth_class;
    std::string role;
    std::uint64_t expires_unix_ms{0};
    [[nodiscard]] std::string routing_key() const;
};

std::optional<ResolveSessionResponse> parse_resolve_response(std::string_view body);

class IAuthServiceClient {
public:
    virtual ~IAuthServiceClient() = default;
    virtual ResolveSessionResponse resolve(const ResolveSessionRequest& request) = 0;
};

} // namespace gateway::auth
```

- [ ] **Step 4: Add gateway configuration**

Add env parsing:

- `AUTH_SERVICE_URL`
- `AUTH_REQUIRED=1` for production-safe mode
- `GATEWAY_TO_SERVER_SIGNING_SECRET`
- `AUTH_TO_GATEWAY_SIGNING_SECRET` only if auth response signing is enabled
- `ALLOW_ANONYMOUS` remains, but means “auth service may issue anonymous principal”, not “skip auth”.

Gateway readiness must fail when `AUTH_REQUIRED=1` and `AUTH_SERVICE_URL` or the gateway-to-server signing secret is missing/default/too short.

- [ ] **Step 5: Replace login admission flow**

In `gateway/src/gateway_connection.cpp`:

1. Parse first `MSG_LOGIN_REQ` as today.
2. Call `auth_client.resolve({user, token, remote_ip, allow_anonymous})`.
3. Use `principal_id` or `auth_session_id` for sticky routing instead of username.
4. Create the gateway-to-server trusted context with `iss`, `aud`, `iat`, `not_after`, `jti`, `kid`, `principal_id`, `auth_session_id`, `display_name`, `auth_class`, and `role`.
5. Forward the frozen internal envelope from Task 0 to the backend; include the original client token/resume token only as a distinct field when the server still needs it.
6. Never forward a client-supplied `gateway-auth-v1:` token as trusted. Re-sign only after successful auth resolution.
7. Keep legacy fallback behind `AUTH_REQUIRED=0` only.

Add tests for:

- `AUTH_REQUIRED=1` and auth service timeout/5xx -> login denied, fail-closed metric increments.
- `AUTH_REQUIRED=0` -> legacy fallback remains available for development only.
- client-supplied internal envelope prefix -> rejected or ignored and re-resolved through auth service.
- anonymous issuance flood/token spray -> throttled or denied once configured limits are exceeded.

- [ ] **Step 6: Add metrics**

Add counters:

- `gateway_auth_resolve_total{result="ok|denied|error"}`
- `gateway_auth_fail_closed_total`
- `gateway_auth_legacy_fallback_total`

- [ ] **Step 7: Run gateway tests**

Run:

```powershell
pwsh scripts/build.ps1 -Config Debug -Target gateway_auth_service_client_tests
ctest --test-dir build-windows -C Debug -R "AuthServiceClient|SessionDirectory" --output-on-failure
```

Expected: auth parser and existing session directory tests pass.

- [ ] **Step 8: Commit**

```powershell
git add gateway tests/gateway
git commit -m "feat: resolve gateway logins through auth service"
```

---

## Task 4: Server Trusted Auth Context

**Files:**
- Create: `server/src/chat/auth_context.hpp`
- Create: `server/src/chat/auth_context.cpp`
- Modify: `server/CMakeLists.txt`
- Modify: `server/src/chat/handlers_login.cpp`
- Modify: `server/src/chat/chat_service_state.hpp`
- Modify: `server/src/chat/chat_command_authorization.cpp`
- Modify: `server/src/chat/handlers_chat.cpp`
- Modify: `server/src/app/config.cpp`
- Test: `tests/server/test_server_chat.cpp`

- [ ] **Step 1: Write failing server login tests**

Add tests to `tests/server/test_server_chat.cpp`:

```cpp
TEST(ChatServiceAuthTest, LoginAcceptsTrustedAnonymousContext) {
    // Arrange trusted context with principal_id/auth_session_id/display_name/auth_class.
    // Send MSG_LOGIN_REQ using the new gateway-forwarded payload.
    // Assert LoginRes effective_user == display_name.
    // Assert new auth fields contain principal_id/auth_session_id/auth_class/role.
    // Assert existing logical_session_id/resume_token keep server continuity semantics.
}

TEST(ChatServiceAuthTest, LoginRejectsMissingTrustedContextWhenRequired) {
    // Configure SERVER_REQUIRE_TRUSTED_AUTH_CONTEXT=1.
    // Send legacy username-only login.
    // Assert MSG_ERR unauthorized.
}

TEST(ChatServiceAuthTest, RejectsExpiredForgedOrReplayedTrustedContext) {
    // Expired, wrong-audience, bad-signature, and replayed jti contexts are rejected.
}

TEST(ChatServiceAuthTest, TwoPrincipalsMayShareDisplayNameWithoutSharingPrivileges) {
    // Same display_name, distinct principal_id.
    // Muting/banning/admin checks must apply to the intended principal only.
}
```

- [ ] **Step 2: Verify tests fail**

Run:

```powershell
pwsh scripts/build.ps1 -Config Debug -Target server_general_tests
ctest --test-dir build-windows -C Debug -R "ChatServiceAuthTest" --output-on-failure
```

Expected: tests fail because trusted context parsing is not implemented.

- [ ] **Step 3: Implement auth context parser**

`server/src/chat/auth_context.hpp`:

```cpp
namespace server::app::chat {

struct AuthenticatedPrincipal {
    std::string principal_id;
    std::string auth_session_id;
    std::string display_name;
    std::string auth_class;
    std::string role;
    std::uint64_t issued_at_unix_ms{0};
    std::uint64_t not_after_unix_ms{0};
    std::string key_id;
    std::string jti;
};

std::optional<AuthenticatedPrincipal> parse_trusted_auth_context(
    std::string_view token,
    std::string_view secret,
    std::uint64_t now_unix_ms);

} // namespace server::app::chat
```

Delegate signature verification to `auth::verify_trusted_context`.
Validation must reject wrong `aud`, expired contexts, unknown/default `kid`, empty/default secrets, replayed `jti`, and direct-to-server attempts that do not carry a gateway-signed envelope.

- [ ] **Step 4: Update login handler**

In `handlers_login.cpp`:

1. Detect new forwarded token format.
2. If required, reject missing/invalid trusted context.
3. Use `display_name` for UI, `principal_id` for ownership, and `auth_session_id` for auth tracing/routing correlation.
4. Stop calling `users().create_guest()` on the trusted path.
5. Populate `LoginRes` fields:
   - `effective_user = display_name`
   - `principal_id = principal_id`
   - `auth_session_id = auth_session_id`
   - `auth_class = auth_class`
   - `role = role`
   - `is_admin = role == admin`
   - `logical_session_id` remains existing server continuity id
   - `resume_token` remains server continuity token if still used

- [ ] **Step 5: Update state model**

In `chat_service_state.hpp`, add maps keyed by session pointer:

```cpp
std::unordered_map<Session*, std::string> principal_id_by_session;
std::unordered_map<Session*, std::string> auth_session_id_by_session;
std::unordered_map<Session*, std::string> auth_class_by_session;
std::unordered_map<Session*, std::string> role_by_session;
```

Keep old name maps for compatibility during migration.

- [ ] **Step 6: Update authorization**

`actor_is_admin_locked` should prefer role from auth context. Keep `CHAT_ADMIN_USERS` fallback only in legacy mode.

Also migrate or guard all name-based privilege surfaces in this slice:

- room ownership and lock/unlock checks
- mute/ban/block keys
- whisper sender/recipient resolution
- audit/session event identity fields

Where a full migration is too large, add compatibility wrappers that resolve display name to principal within the current room/session and add tests that duplicate display names do not share privileges.

- [ ] **Step 7: Run tests**

Run:

```powershell
pwsh scripts/build.ps1 -Config Debug -Target server_general_tests
ctest --test-dir build-windows -C Debug -R "ChatServiceAuthTest|ChatServiceTest.Login|ChatServiceTest.ChatSend" --output-on-failure
```

Expected: new auth tests and legacy login compatibility tests pass.

- [ ] **Step 8: Commit**

```powershell
git add server tests/server
git commit -m "feat: consume trusted auth context in chat server"
```

---

## Task 5: Client Auth State And Login UI

**Files:**
- Modify: `client_gui/include/client/client_app.hpp`
- Modify: `client_gui/src/client_app.cpp`
- Modify: `client_gui/include/client/net_client.hpp`
- Modify: `client_gui/src/net_client.cpp`
- Modify: `client_gui/include/client/gui_manager.hpp`
- Modify: `client_gui/src/gui_manager.cpp`
- Modify: `client_gui/README.md`
- Test: `tests/python/test_client_wire_contracts.py` or `tests/client/test_net_client_auth_contract.cpp`

- [ ] **Step 1: Write failing client contract tests**

Prefer a C++ test or loopback harness that exercises the real `NetClient` decode/callback path. Create `tests/client/test_net_client_auth_contract.cpp` if the chosen preset can build client tests. Use `tests/python/test_client_wire_contracts.py` only as a temporary fallback, and do not rely on a Python packet mirror as the only proof for callback propagation.

Minimum assertions:

- Existing login request path can include a non-empty client access/resume token without changing wire compatibility.
- `LoginRes` auth fields `principal_id`, `auth_session_id`, `auth_class`, `role` are preserved by the client-facing decode helper.
- Existing `LoginRes` continuity fields `logical_session_id`, `resume_token`, `resume_expires_unix_ms`, `resumed`, `world_id` are preserved with their current meanings.
- Receive body larger than configured max is rejected.
- Command/chat send remains blocked until login succeeds.
- A user-entered token with `gateway-auth-v1:` internal prefix is rejected client-side before send.

- [ ] **Step 2: Verify tests fail**

Run:

```powershell
python tests/python/test_client_wire_contracts.py
```

Expected: fails because decode helper/state fields do not exist, or because the expected fields are ignored.

- [ ] **Step 3: Add client auth/session state**

In `client_app.hpp`:

```cpp
struct AuthState {
    bool anonymous_mode = true;
    std::string access_token;
    std::string principal_id;
    std::string auth_session_id;
    std::string auth_class;
    std::string role;
    std::string logical_session_id;
    std::string resume_token;
    std::uint64_t resume_expires_unix_ms = 0;
    bool resumed = false;
};
```

Add `AuthState auth;` to `AppData`.

- [ ] **Step 4: Expand NetClient login callback**

Change callback to:

```cpp
using OnLoginRes = std::function<void(LoginResult result)>;

struct LoginResult {
    std::string effective_user;
    std::uint32_t sid = 0;
    bool is_admin = false;
    std::string principal_id;
    std::string auth_session_id;
    std::string auth_class;
    std::string role;
    std::string logical_session_id;
    std::string resume_token;
    std::uint64_t resume_expires_unix_ms = 0;
    bool resumed = false;
    std::string world_id;
};
```

- [ ] **Step 5: Preserve all `LoginRes` fields**

In `net_client.cpp`, update `MSG_LOGIN_RES` handling to fill `LoginResult` from protobuf. Leave legacy fallback but mark missing fields empty.

- [ ] **Step 6: Add login UI controls**

In `GuiState`:

```cpp
bool anonymous_mode = true;
char token_buffer[512] = "";
bool remember_session = false;
```

In login modal:

- Checkbox: `Anonymous`
- Token field disabled when anonymous is checked unless user chooses “Use existing token”
- Display current auth class/session after login

- [ ] **Step 7: Pass token mode through existing login path**

`NetClient::send_login` already has a token parameter. Wire `GuiManager` -> `ClientApp` -> `NetClient` so the UI can choose:

- anonymous: send display name plus empty token; gateway/auth service issues identity.
- token: send display name plus client access token.
- resume: send display name plus the existing server resume-token format if the current continuity contract requires it.

Do not let the client construct or persist gateway-to-server internal contexts.

- [ ] **Step 8: Add network safety limits**

In `NetClient`:

- `static constexpr std::size_t kMaxReceiveBodyBytes = 64 * 1024;`
- `static constexpr std::size_t kMaxQueuedBytes = 256 * 1024;`

Reject frames exceeding max before `read_body_.assign`.

- [ ] **Step 9: Run client build and tests**

Run:

```powershell
pwsh scripts/build.ps1 -ClientOnly -Target client_gui
python tests/python/test_client_wire_contracts.py
```

Expected: client builds; wire contract test passes.

- [ ] **Step 10: Commit**

```powershell
git add client_gui tests/python/test_client_wire_contracts.py
git commit -m "feat: add auth session state to client"
```

---

## Task 6: End-To-End Anonymous Auth Chat

**Files:**
- Modify: `tests/python/verify_anonymous_auth_chat.py`
- Modify: `tests/python/run_stack_test.py`
- Modify: `docker/stack/docker-compose.yml`
- Modify: `scripts/generate_stack_topology.py`
- Modify: `docker/stack/topologies/default.json` only if server direct host ports are removed or changed.

- [ ] **Step 1: Extend e2e test**

`verify_anonymous_auth_chat.py` should:

1. Connect only to HAProxy/gateway; do not call `auth_app` directly in this e2e.
2. Send `MSG_LOGIN_REQ` with display name and empty token for anonymous mode.
3. Assert `MSG_LOGIN_RES` includes `effective_user`, new auth fields (`principal_id`, `auth_session_id`, `auth_class`, `role`), and existing continuity fields (`logical_session_id`, `resume_token` when enabled).
4. Send chat message.
5. Assert broadcast sender is display name while principal/session identity remains stable in returned state/events.
6. Disconnect/reconnect with server resume token if available.
7. Stop or misconfigure `auth-app`, attempt a fresh login with `AUTH_REQUIRED=1`, and assert login is denied through gateway.

- [ ] **Step 2: Verify it fails before stack wiring**

Run:

```powershell
python tests/python/verify_anonymous_auth_chat.py --host 127.0.0.1 --port 36000
```

Expected: fails until stack is running with `auth_app`.

- [ ] **Step 3: Wire stack dependency order**

In compose:

- `gateway-*` depends on `auth-app: condition: service_healthy`
- `server-*` gets `GATEWAY_TO_SERVER_SIGNING_SECRET` or equivalent server-side verification key
- `gateway-*` gets `AUTH_SERVICE_URL=http://auth-app:39100`, `AUTH_REQUIRED=1`, `GATEWAY_TO_SERVER_SIGNING_SECRET`
- Remove default direct server host port publishing for production profile; keep dev override if necessary.

- [ ] **Step 4: Update topology generator**

`scripts/generate_stack_topology.py` must render new server auth env and avoid exposing server TCP ports by default unless `dev_expose_tcp` is set in topology.

- [ ] **Step 5: Run generated topology test**

Run:

```powershell
python tests/python/verify_stack_topology_generator.py
python scripts/generate_stack_topology.py --topology-config docker/stack/topologies/default.json --output-compose $env:TEMP\auth-topology.yml --output-active $env:TEMP\auth-topology.json
```

Expected: topology tests pass; generated compose includes auth env and omits public server TCP ports unless explicitly enabled.

- [ ] **Step 6: Run full stack e2e**

Run:

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build
python tests/python/run_stack_test.py tests/python/verify_anonymous_auth_chat.py --host 127.0.0.1 --port 36000
pwsh scripts/deploy_docker.ps1 -Action down
```

Expected: anonymous auth chat path passes.

- [ ] **Step 7: Commit**

```powershell
git add docker/stack scripts tests/python
git commit -m "test: verify anonymous auth chat stack"
```

---

## Task 7: Data Ownership Baseline

**Files:**
- Create: `tools/migrations/0002_auth_schema.sql`
- Create: `tools/migrations/0003_chat_identity_columns.sql`
- Modify: `tools/migrations/runner.cpp`
- Modify: `tools/migrations/README.md`
- Modify: `server/src/storage/postgres/connection_pool.cpp`
- Modify: `server/include/server/storage/repositories.hpp`
- Test: `tests/test_storage_basic.cpp`

- [ ] **Step 1: Write failing storage tests**

Add tests proving:

- Auth principals/sessions can be created independently.
- Chat messages can store `principal_id` and `sender_display_name`.
- Dev seed migration is not applied in production mode.

- [ ] **Step 2: Verify tests fail**

Run:

```powershell
pwsh scripts/build.ps1 -Config Debug -Target storage_basic_tests
ctest --test-dir build-windows -C Debug -R "Storage" --output-on-failure
```

Expected: tests fail because schemas/repositories are missing.

- [ ] **Step 3: Add schemas**

`0002_auth_schema.sql`:

```sql
create schema if not exists auth;

create table if not exists auth.principals (
  id uuid primary key,
  auth_class text not null check (auth_class in ('anonymous', 'registered')),
  display_name text not null,
  created_at timestamptz not null default now(),
  last_seen_at timestamptz
);

create table if not exists auth.sessions (
  id uuid primary key,
  principal_id uuid not null references auth.principals(id) on delete cascade,
  token_hash bytea not null unique,
  created_at timestamptz not null default now(),
  expires_at timestamptz not null,
  revoked_at timestamptz
);
```

`0003_chat_identity_columns.sql`:

```sql
alter table messages add column if not exists principal_id uuid;
alter table messages add column if not exists sender_display_name text;
alter table memberships add column if not exists principal_id uuid;
```

- [ ] **Step 4: Add migration mode**

Add env or flag:

- `MIGRATIONS_INCLUDE_DEV_SEED=0` default
- Only apply files matching `0xxx_*.sql` by default; apply `0100_seed_dev.sql` only when dev seed is enabled.

- [ ] **Step 5: Keep repositories minimal**

Do not rewrite all storage. Add only methods needed by auth/chat identity tests.

- [ ] **Step 6: Run storage tests**

Run:

```powershell
pwsh scripts/build.ps1 -Config Debug -Target storage_basic_tests
ctest --test-dir build-windows -C Debug -R "Storage" --output-on-failure
```

Expected: storage tests pass.

- [ ] **Step 7: Commit**

```powershell
git add tools/migrations server/src/storage server/include/server/storage tests/test_storage_basic.cpp
git commit -m "feat: split auth and chat data ownership"
```

---

## Task 8: Durable Events And Audit Safety

**Files:**
- Create: `tools/migrations/0004_outbox_inbox_minimal.sql`
- Modify: `server/src/chat/chat_service_core.cpp`
- Modify: `server/src/chat/session_events.cpp`
- Modify: `tools/wb_worker/main.cpp`
- Modify: `infra/src/storage/redis/client.cpp`
- Modify: `docs/db/write-behind.md`
- Test: `scripts/smoke_wb.ps1`
- Test: `tests/python/verify_anonymous_auth_chat.py`

- [ ] **Step 1: Write failing event safety tests**

Add checks for:

- Consumer group starts from `0` or an explicit safe bootstrap mode.
- `XADD` failure increments visible error and does not silently claim success.
- `WB_ACK_ON_ERROR=1` and `WB_DLQ_ON_ERROR=0` is rejected in production mode.

- [ ] **Step 2: Fix Redis group creation**

In `infra/src/storage/redis/client.cpp`, stop hardcoding `$`. Provide:

```cpp
bool xgroup_create_mkstream(const std::string& key, const std::string& group, std::string_view start_id);
```

Default worker start id should be `0` unless explicitly configured.

- [ ] **Step 3: Fix producer failure behavior**

In chat write-behind emission, make `XADD` failure:

- visible in metrics/logs
- optionally backpressure or disable write-behind after threshold
- never appear as successful persistence

- [ ] **Step 4: Add minimal outbox**

Create `chat.outbox_events` and `auth.outbox_events`, but only route session audit events through the existing worker in this tranche. Keep Pub/Sub for live chat fanout.

- [ ] **Step 5: Run write-behind smoke**

Run:

```powershell
pwsh scripts/smoke_wb.ps1
```

Expected: smoke passes and verifies no silent ACK-on-error data loss configuration.

- [ ] **Step 6: Commit**

```powershell
git add infra server tools/wb_worker tools/migrations docs/db scripts tests
git commit -m "fix: harden durable chat audit events"
```

---

## Task 9: Admin And Operations Hardening For The New Boundary

**Files:**
- Modify: `tools/admin_app/main.cpp`
- Modify: `tools/admin_app/README.md`
- Modify: `docker/stack/docker-compose.yml`
- Modify: `docker/stack/README.md`
- Modify: `docs/configuration.md`
- Modify: `.github/workflows/ci-stack.yml`
- Test: `tests/python/verify_admin_auth.py`
- Test: `tests/python/verify_admin_read_only.py`
- Test: `tests/python/test_ci_workflow_contracts.py`

- [ ] **Step 1: Write failing config tests**

Extend existing Python tests to assert:

- `ADMIN_AUTH_MODE` is not `off` in stack default.
- `ADMIN_READ_ONLY=1` unless write mode is explicitly enabled.
- Metrics/admin ports are not broadly published in production profile.
- `scripts/generate_stack_topology.py` changes trigger stack CI.

- [ ] **Step 2: Verify failures**

Run:

```powershell
python tests/python/verify_admin_auth.py
python tests/python/verify_admin_read_only.py
python tests/python/test_ci_workflow_contracts.py
```

Expected: tests fail on current defaults.

- [ ] **Step 3: Harden admin defaults**

Set compose defaults:

- `ADMIN_AUTH_MODE=bearer`
- `ADMIN_READ_ONLY=1`
- no dev signing secret fallback in production profile

Keep a documented dev override for local use.

- [ ] **Step 4: Fix docs drift**

Update docs to state:

- Client -> HAProxy -> gateway -> server only
- Auth service is required in production
- Direct server ports are dev-only
- Anonymous means issued anonymous principal, not unauthenticated access

- [ ] **Step 5: Run tests**

Run the same Python tests from Step 2.

Expected: tests pass.

- [ ] **Step 6: Commit**

```powershell
git add tools/admin_app docker/stack docs .github tests/python
git commit -m "chore: harden auth stack operations defaults"
```

---

## Task 10: Final Verification Matrix

**Files:**
- Modify: `tasks/architecture/msa-runtime-audit/todo.md`
- Modify: `tasks/architecture/chat-client-structure-audit/todo.md`
- Create or modify: `docs/ops/authenticated-chat-stack.md`

- [ ] **Step 1: Run generation checks**

```powershell
python tools/gen_opcode_docs.py --check
python tools/check_core_api_contracts.py --check-boundary
```

Expected: both pass.

- [ ] **Step 2: Run targeted CTest**

```powershell
ctest --preset windows-test -R "Auth|Gateway|ChatService|Storage|CorePublicApi" --output-on-failure
```

Expected: auth/gateway/chat/storage tests pass. If pre-existing installed package consumer failures remain, record them separately and do not hide them.

- [ ] **Step 3: Run client build**

```powershell
pwsh scripts/build.ps1 -ClientOnly -Target client_gui
```

Expected: client builds in `build-windows-client`.

- [ ] **Step 4: Run stack e2e**

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build
python tests/python/run_stack_test.py tests/python/verify_anonymous_auth_chat.py --host 127.0.0.1 --port 36000
pwsh scripts/deploy_docker.ps1 -Action down
```

Expected: anonymous auth chat e2e passes and stack shuts down cleanly.

- [ ] **Step 5: Document final state**

Create `docs/ops/authenticated-chat-stack.md` with:

- service map
- auth/session flow
- anonymous identity rules
- client login modes
- failure modes
- local dev commands
- production hardening checklist

- [ ] **Step 6: Commit**

```powershell
git add docs
git commit -m "docs: document authenticated chat stack"
```

---

## Execution Notes

- Use @superpowers:test-driven-development for each implementation task.
- Use @superpowers:verification-before-completion before claiming each task done.
- Use subagents by ownership:
  - Auth service and contract: backend-developer or cpp-pro.
  - Gateway auth adapter: websocket-engineer or network-engineer.
  - Server chat auth context: cpp-pro.
  - Client GUI auth state: cpp-pro or frontend-adjacent worker with ImGui context.
  - Data/event migrations: data-engineer.
  - Compose/CI/docs: devops-engineer.
- Keep compatibility flags until the new path passes e2e:
  - `AUTH_REQUIRED=0` legacy mode for local rollback.
  - `SERVER_REQUIRE_TRUSTED_AUTH_CONTEXT=0` until gateway path is live.
  - Then flip defaults in the hardening task.

## Completion Criteria

- Anonymous user receives service-issued `principal_id` and `auth_session_id`.
- Client displays auth/session state and preserves resume fields.
- Gateway refuses unauthenticated production login when auth service is unavailable.
- Server does not trust client-provided display name as identity on the new path.
- Chat send/join/whisper still work through HAProxy -> gateway -> server.
- Auth/chat/audit data ownership is separated at least by schema and code path.
- Stack e2e proves anonymous auth chat with the updated client contract.
