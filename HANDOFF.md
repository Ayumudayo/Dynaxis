# HANDOFF: Chat Client/Server Rework (FTXUI + Protocol)

## Purpose
- Hand-off note so the next session can resume work quickly.
- Includes goals, design, verification checklist, remaining tasks, and acceptance criteria.

## Top Goals
- Auto-connect to `localhost:5000`; remove `/connect` and `/disconnect`.
- FTXUI client with status bar, left rooms, right logs, bottom input, F1 help overlay.
- Server/client sync for rooms/users with a single `/refresh`.
- Update UI even when window unfocused.
- Correct “me” detection via `sender_sid` (with capability negotiation).
- Block guest chat; assign 8-digit hex random nick if none on login.
- Room preview (click room → fetch users).
- Remove unnecessary buttons; rely on keyboard/commands.
- Refactor to SOLID: server `ChatService`, client `NetClient` + `Store`.
- Update docs, then commit.

## Architecture Decisions

### Server
- Introduce `ChatService` for domain logic: login, join/leave, send chat, snapshot/list, and broadcasting.
- `main` only registers handlers and wires transports to `ChatService`.
- Track `Session.session_id()` via `SharedState.next_session_id`.

### Client
- `NetClient`: async transport (e.g., asio), callbacks: `on_hello/on_err/on_login_res/on_broadcast/on_room_users/on_snapshot`. Public `send_*` APIs.
- `Store`: single source of truth (connected, username, current_room, preview_room, rooms, users, logs, my_sid, cap_sender_sid).
- FTXUI UI reads `Store` snapshot; `NetClient` callbacks update `Store` and trigger `screen.PostEvent(Event::Custom)` to re-render.

### Protocol
- Messages:
  - `MSG_HELLO(0x0001)`, `MSG_PING(0x0002)`, `MSG_PONG(0x0003)`, `MSG_ERR(0x0004)`
  - `MSG_LOGIN_REQ(0x0010)`, `MSG_LOGIN_RES(0x0011)` with `effective_user`, `session_id`
  - `MSG_CHAT_SEND(0x0100)`, `MSG_CHAT_BROADCAST(0x0101)` with `sender_sid` if `CAP_SENDER_SID`
  - `MSG_JOIN_ROOM(0x0102)`, `MSG_LEAVE_ROOM(0x0103)`
  - `MSG_STATE_SNAPSHOT(0x0200)` with `current_room + rooms + users`
  - `MSG_ROOM_USERS(0x0201)` for room user list
- Capabilities/flags:
  - `CAP_SENDER_SID(0x0002)`
  - `FLAG_SELF(0x0100)` kept temporarily for backward compatibility
- Errors:
  - `INTERNAL_ERROR(0x0001)`, `INVALID_PAYLOAD(0x0007)`
  - `NAME_TAKEN(0x0100)`, `UNAUTHORIZED(0x0101)`, `NO_ROOM(0x0104)`, `NOT_MEMBER(0x0105)`, `ROOM_MISMATCH(0x0106)`

## Quick Verification Checklist

### Server layout
- `rg "class ChatService|struct ChatService|ChatService"` → confirm service boundary exists.
- `rg "next_session_id|session_id\("` → confirm session id generator and accessor.
- `rg "MSG_LOGIN_RES|effective_user|session_id"` → confirm login response fields.

### Client layout
- `rg "class NetClient|struct NetClient"` → confirm transport wrapper exists.
- `rg "class Store|struct Store"` → confirm single state holder exists.
- `rg "screen.PostEvent|PostEvent\(Event::Custom"` → confirm UI refresh path on network events.
- `rg "CAP_SENDER_SID|sender_sid|FLAG_SELF"` → confirm “me” detection logic.
- `rg "F1|help|overlay"` → confirm help overlay wiring.
- `rg "/refresh|snapshot|MSG_STATE_SNAPSHOT"` → confirm refresh flow.

### UI/UX
- Ensure no `/connect` `/disconnect` commands and auto-connect to `localhost:5000`.
- Left panel: rooms; click → `preview_room` and fetch via `MSG_ROOM_USERS`.
- Right panel: logs; “me” messages styled distinctly.
- Status bar: show connection, user, room; guest highlighted (e.g., yellow).
- Input: command parsing for `/refresh`, `/join <room>`, `/leave`, `/who [room]`.

## What To Implement Next (High Priority)
- Client refactor completion
  - Remove any legacy direct asio/recv threads still in `main.cpp`; route all I/O via `NetClient`.
  - Ensure UI reads only from `Store`; callbacks mutate `Store` and trigger `PostEvent`.
  - Centralize message handlers in a table-like dispatch to improve OCP.
- Capability-based “me” detection
  - Prefer `sender_sid == Store.my_sid` when `Store.cap_sender_sid` is true.
  - Fallback to `FLAG_SELF` only when capability missing.
- Guest policy
  - Server: assign 8-digit hex nick on login if none; deny `MSG_CHAT_SEND` for guests.
  - Client: prevent sending when `Store.username` is guest; show hint to login/rename.
- Snapshots and refresh
  - Implement `/refresh`: request `MSG_STATE_SNAPSHOT`; reconcile `Store.rooms/users/current_room`.
  - Update room/user lists immediately on broadcast or snapshot without focus.
- Room preview
  - On room list click/hover: set `preview_room`, send `MSG_ROOM_USERS(preview_room)`, render inline preview list.
- Help overlay and shortcuts
  - F1 toggles overlay with shortcuts: Join/Leave/Refresh/Who/Send/Scroll.
  - Remove unused buttons; update footer hints accordingly.

## Server Work Items
- Add `ChatService` with methods:
  - `on_login`, `on_join`, `on_leave`, `on_chat_send`, `on_session_close`.
  - `broadcast_room`, `send_room_users`, `send_rooms_list`, `send_snapshot`.
- `MSG_LOGIN_RES`: include `effective_user`, `session_id`; set `CAP_SENDER_SID` in `HELLO` or `LOGIN_RES`.
- Enforce errors:
  - `UNAUTHORIZED` for guest sending chat.
  - `NO_ROOM/NOT_MEMBER/ROOM_MISMATCH` as applicable.
- Random nick generation:
  - 8 hex chars; ensure uniqueness at time of assignment; collision retry.

## Client Work Items
- `NetClient`
  - `set_on_hello/on_err/on_login_res/on_broadcast/on_room_users/on_snapshot`.
  - `send_login(optional_nick)`: send empty nick to request server-assigned guest nick.
  - `send_join/leave/chat/refresh/who`.
- `Store`
  - Fields: `connected, username, is_guest, current_room, preview_room, rooms{meta}, users{by room}, logs, my_sid, cap_sender_sid`.
  - Reducer-like update helpers; expose immutable snapshot for UI render.
- UI
  - Status bar: connection LED, user, room; adapt to width.
  - Left rooms: selectable; preview inline user list for `preview_room`.
  - Right logs: colorize “me”, errors; timestamps; scrollable.
  - Bottom input: parse `/join`, `/leave`, `/who`, `/refresh`; show error hints.
  - F1 overlay: keyboard shortcuts and command cheatsheet.
- Eventing
  - From `NetClient` callbacks: update `Store`; call `screen.PostEvent(Event::Custom)`.

## Acceptance Criteria
- App auto-connects to `localhost:5000` on start; no `/connect` command available.
- Logging in without a nick yields an 8-hex guest nick; guest cannot send messages.
- Pressing F1 shows/hides help overlay; footer shows concise shortcut hints.
- `/refresh` updates rooms/users/current room in UI immediately, even when app was unfocused.
- Room click shows user preview; `/who <room>` shows structured list in logs.
- “Me” marking is correct with `CAP_SENDER_SID`; falls back to `FLAG_SELF` otherwise.
- No unused buttons present; keyboard-only operation viable.

## Testing Guide

### Manual
- Start server and client; observe auto-connect and guest nick assignment.
- As guest, attempt to chat → UI shows error with `UNAUTHORIZED`.
- Join a room, send chat after named login → message marked as “me”.
- Click different rooms → preview users; run `/who <room>` and compare.
- Run `/refresh` then verify rooms/users reflect server state.

### Programmatic (optional, if test harness exists)
- Unit-test `ChatService` methods for error codes and broadcasts.
- Unit-test `Store` reducers for merge logic on snapshot and broadcasts.

## Implementation Hints
- Random nick
  - Use a secure PRNG if available; otherwise `random_device` + `mt19937`; format to 8 hex chars.
- Threading
  - Ensure `NetClient` callbacks do not directly mutate UI state from non-UI threads; use a thread-safe queue or post to UI thread then `PostEvent`.
- Reconciliation
  - On `MSG_STATE_SNAPSHOT`, replace room list wholesale; diff users per room to minimize UI churn if needed.
- Error surface
  - Map error codes to short, user-friendly Korean messages; log both code and hint.

## Runbook
- Server
  - Build and run: `make server` or `cargo run -p server` or `bazel run //server` (adjust to repo).
  - Confirm listening on `localhost:5000`.
- Client
  - Build and run: `make client` or `cargo run -p client` or `bazel run //client`.
  - Client should auto-login as guest on connect.

## Useful greps
- `rg "MSG_STATE_SNAPSHOT|MSG_ROOM_USERS"`
- `rg "CAP_SENDER_SID|sender_sid"`
- `rg "PostEvent\\(Event::Custom"`

## Open Questions To Resolve
- Exact transport and message serialization format (JSON/CBOR/custom). If not fixed, standardize now.
- Where to declare capability flags (in HELLO vs LOGIN_RES). Prefer HELLO, but LOGIN_RES acceptable if simpler.
- UI color scheme for guest and self messages; decide consistent palette.

## First 30 Minutes Next Session
- Build server/client; run smoke test for auto-connect and guest nick.
- `rg` the checklist to confirm `ChatService`, `NetClient`, `Store` presence.
- If legacy networking remains in `main.cpp`, remove and wire through `NetClient`.
- Implement `/refresh` end-to-end if missing; verify UI updates on unfocused window via `PostEvent`.
- Hook F1 overlay and footer hints; remove stray buttons.

