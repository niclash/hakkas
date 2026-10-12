# Implementation plan

The order in which hakkas gets built: each task is one self-contained change with unit tests,
committed on `main`. The specifications stay the source of truth; this file only sequences
the work. Mark a task `[x]` when its commit lands.

Workflow: tasks run one after another, each in a fresh agent; each is reviewed (tests, diff)
before the next starts. The chain stops on a failed review, an open question, or a decision.

Tasks marked **blocked** depend on an open question in `specifications/00-README.md` and wait
for a decision.

## Phase 1 — Engine core (no networking)

- [x] **1. `Linkable` value type** (`src/graphs/`): the tagged value, `Metric` struct, the
  converters `to_f64`/`to_i64`/`to_u64`/`to_bool`/`default_value`, stringify, string parsing
  per port type (01 §2.1, 02 §3.4 "Initial value encoding"). NaN/±Inf/out-of-range → integer
  conversion: pick the cheapest well-defined behavior and record it in 01 §2.1 (open question
  8 explicitly delegates this). Metric arithmetic rules stay out (open question 9).
- [x] **2. Logging** (`src/system/`): four levels, threshold, file/line tagging, a sink hook
  for later remote mirroring (05 §4).
- [x] **3. PAL interface + POSIX clock/random/storage** (`src/platform/pal.h`,
  `src/platform/posix/`): monotonic ms clock, cryptographic random bytes, storage
  read/write/rename/remove/exists/list with the atomic-write procedure and `.tmp` cleanup
  (01 §0, 05 §2.3).
- [x] **4. Work queue** (`src/graphs/`): fixed-capacity ring buffer of typed entries,
  block handles (slot + generation), per-input source generation, drop-oldest with a drop
  counter (01 §3.2). Capacity: fixed, default 1024 entries, overridable per build (decided).
- [x] **5. Block type registry + built-in math/process types** (`src/blocktypes/`):
  `BlockTypeDef`, registry lookup, every stateless type in 03 §3 table (not `timing/`, not
  `io/`), with compute-function unit tests.
- [x] **6. Generic block runtime + Graph model** (`src/graphs/`): blocks, inputs/outputs,
  links, initials, no fan-in, severing, change threshold resolution, start/stop/destroy,
  `output_set`, the drain loop, "block.port" parsing (01 §2.2–§3.3, 03 §2).
- [x] **7. Graphs registry + event notification** (`src/graphs/`): graph create/delete/
  rename, ports (inports/outports), `GraphNotify` subscriber fan-out, invalid-command
  refusals with the exact messages of 02 §3.4.1, uptime accounting (01 §2.2, §4).
- [x] **8. Timers + `timing/Interval`**: timers as core deadlines in a min-heap, fired by the
  engine loop (no PAL timer: the loop's poll timeout is the only timing primitive, 01 §3.2
  item 7), timer-expiry queue entries, `TIMER_PERIODIC`/`TIMER_SCHEDULED` handling, the
  Interval block (03 §3.1).

## Phase 2 — Persistence and configuration

- [x] **9. JSON library integration**: vendor a pinned cJSON release into `third_party/cjson/`
  (decided, 01 §6) and add it to the core build.
- [x] **10. Graph file load/save** (`src/app/`): 05 §2.1–§2.3 format, file-name encoding,
  placeholder blocks for unknown types, round-trip tests. cJSON does not print every double
  exactly: write f64 values via `linkable_stringify` + `cJSON_CreateRaw`, and decide how a
  non-finite initial is stored (`third_party/cjson/README.md`, "Numbers").
- [x] **11. Configuration file + POSIX CLI** (`src/system/`, `src/platform/posix/`): `hakkas.json`
  keys and defaults (05 §1.1), command tree `list types`, `describe type|topology`,
  `run process|daemon` (05 §1.2).

## Phase 3 — Network and protocol

- [x] **12. PAL sockets + event loop** (POSIX `poll`): the single loop of 01 §3.2.1 —
  sockets, firing due timers (`runtime_fire_timers`, poll timeout from
  `runtime_timer_timeout_ms`), queue drain. Done: PAL sockets, `pal_poll`, the cross-context
  handoff (`pal_handoff_push`) and stop request (01 §0.2); the portable loop
  `src/engine/loop.h` with pluggable listeners/connections, bounded outgoing buffers and tick
  hooks for other deadlines (01 §3.2.1); POSIX `SIGINT`/`SIGTERM` handlers
  (`platform/posix/signals.h`).
- [x] **13. HTTP static server** (05 §3): GET only, SPA fallback, `.gz` variants. Build it on
  the loop (`engine/loop.h`): `loop_listen` with an `on_accept` that sets a `LoopConnHandler`;
  stream files through `on_drained`/`loop_conn_space`, pause reading with
  `loop_conn_pause_read` while a response is pending, `loop_conn_close` after the response;
  choose the listener's `out_cap` and a request-header limit, and an idle timeout via a tick
  hook (`loop_add_tick`) if one is wanted. Done: the incremental HTTP/1.1 request-head parser
  `src/protocol/http.h` (8 KiB / 32 fields, header lookup, token lists, keep-alive,
  `Accept-Encoding`), the web server `src/engine/web_server.h` (GET and HEAD, secure path
  mapping, SPA fallback for extension-less paths, `.gz` variants, keep-alive, timeouts via a
  tick hook), the PAL streaming read `pal_file_open`/`read`/`close` (01 §0.1); the loop gained
  `loop_remove_tick`, `loop_now_ms` and a listener `on_closed` callback. Decisions: 05 §3.
Order change: task 15 (crypto) was done **before** task 14, because the WebSocket handshake needs
SHA-1 and base64 for `Sec-WebSocket-Accept` and the specs forbid hand-written cryptographic
primitives (01 §6). Task 14 uses `src/system/crypto.h` (`crypto_sha1`, `crypto_base64_encode`;
`tests/test_crypto.c` checks the RFC 6455 §1.3 example).

- [x] **14. WebSocket layer** (RFC 6455): upgrade, Origin check, connection and frame-size
  limits (02 §0). On the loop: `max_connections` is counted with `loop_listener_conn_count`
  (refuse the upgrade with 503 above it; set the listener's hard `max_conns` somewhat higher
  so the 503 can be sent); choose `out_cap` (a full outgoing buffer closes the connection,
  01 §3.2.1). Parse the upgrade request with `src/protocol/http.h` (task 13): read into
  `http_request_space`/`http_request_commit`; on `HTTP_PARSE_COMPLETE` the fields are there
  (`http_request_header`, `http_request_header_count` for exactly one `Sec-WebSocket-Key`,
  `http_request_header_has_token` for `Connection: Upgrade` / `Upgrade: websocket`), and the
  bytes after the head (`http_request_leftover`) are already WebSocket frames. A parse error's
  status is `http_parse_status`; the web server's error-and-close path (discard the buffered
  input before `loop_conn_close`, `engine/web_server.c`) avoids a reset. The head buffer can
  be freed after the upgrade (`http_request_init` takes the caller's buffer). The web server
  uses one of the loop's 4 tick hooks. Done: the socket-free pieces `src/protocol/websocket.h`
  (accept key, frame header codec, close codes, an incremental UTF-8 validator, the own-origin
  check) and the server `src/engine/ws_server.h` on the loop (upgrade, Origin check,
  `max_connections`/503, framing and reassembly, close handshake, server pings, a callback API
  for the protocol layer, `ws_server_open_config`); the loop gained `loop_conn_set_out_cap`
  and `LOOP_TICKS_MAX` went from 4 to 8. Decisions: 02 §0 (upgrade, Origin rules, a missing
  Origin is 403, what counts toward the limit, outgoing buffer sizing and its MCU cost, TLS
  refused for now).
- [x] **15. Crypto primitives**: SHA-256, HMAC-SHA256, PBKDF2, base64/base64url, constant-time
  compare, via mbedTLS (decided, 01 §6). Done before task 14 (see above): a vendored mbedTLS
  3.6.7 subset with hakkas' own configuration (`third_party/mbedtls/README.md`) under the thin
  API `src/system/crypto.h` (also SHA-1, `crypto_zeroize`, strict decoders), which replaced task
  11's `system/base64.h`. `tests/test_crypto.c` checks the RFC 7677 SCRAM-SHA-256 example end to
  end (SaltedPassword … ServerSignature and the server's proof check), the chain task 16 uses
  with 02 §2.3's AuthMessage.
- [x] **16. Authorization**: users file, SCRAM handshake, sessions, roles/capabilities, rate
  limiting, `hakkas user add|passwd|del|list` (02 §2). The CLI (task 11, `platform/posix/cli.c`)
  parses `user <add|passwd|del|list>` and refuses it as "not implemented yet": implement the
  subcommands and their arguments here. The users file path is `Config.auth.users_file`.
  Session expiry and login back-off are deadlines: run them from a tick hook
  (`loop_add_tick`, 01 §3.2.1); rate limiting keys on `loop_conn_peer(conn)->addr`
  (on a WebSocket: `ws_peer(conn)->addr`). Session expiry gets its own tick hook (8 are
  available; the WebSocket server's covers only its handshake, ping and close deadlines). The
  session idle timeout counts client messages (`on_message`), not the pongs to the WebSocket
  server's pings (02 §0, "Liveness"). The session lives in the connection's user context
  (`ws_set_user`) and ends in `on_close` (02 §2.5 item 2). Done: the credential store
  `src/protocol/users.h` (records, strict parsing and printing, key derivation,
  `users_load`/`users_load_startup`/`users_save` through the PAL in `users_file.c`, at most `HAKKAS_USERS_MAX` = 32 users), the
  authorizer `src/protocol/auth.h` (the `Authorizer` interface `AuthOps` and the real one,
  `auth_create`; per-connection state, challenge/login with 02 §2.3's AuthMessage, sessions
  with idle and lifetime expiry from the tick hook `auth_tick`, roles → capabilities,
  `auth_command_rule`/`auth_permits` for every command, the rate limiter with deferred
  challenges, logging), the POSIX password reader `platform/posix/password.h` (echo off on a
  terminal, one line otherwise) and the `user` subcommands in `platform/posix/cli.c`
  (`cli_main`/`cli_execute` now take the input stream). Decisions: 02 §2.2–§2.5 (strict file
  format, names without control characters, a malformed file → no users but the engine
  starts, the file read at startup only — changed by task 18: re-read at every challenge —, challenge and login rules, what counts as a failure,
  close code 1008 after 3 failures, back-off counted from the last failure and the deferred
  challenge, the interface's extra operations, log lines), 05 §1.2 (commands, password
  input), 05 §1.4 step 3.
- [x] **17. Protocol dispatcher**: envelope validation, error format, secret gating,
  capability checks, `environment`/`runtime`/`component`/`trace` (02 §1, §3.1–§3.2, §3.5–§3.6).
  Built on `src/engine/ws_server.h` (task 14): pass a `WsHandler` (`on_open` allocates the
  per-connection state and stores it with `ws_set_user`; `on_message` gets one complete,
  valid UTF-8 text message, NUL-terminated, valid during the call only; `on_close` frees the
  state, once per opened connection, after which the `WsConn` is gone; `on_drained`) and its
  context to `ws_server_open_config`. Send with `ws_send_text` (a message that does not fit
  the outgoing buffer closes the connection, 01 §3.2.1, so check `ws_send_space` before a
  burst), close with `ws_close(conn, code, reason)`. The server never looks at message
  contents and never logs them.
  Authorization (task 16, `src/protocol/auth.h`, whose header comment shows the flow): take an
  `Authorizer` (the real one from `auth_authorizer`, a fake in tests). `on_open`:
  `ops->open(self, ws_peer(conn)->addr, &events, conn_state)` (NULL: close the connection);
  `on_close`: `ops->close`. For **every** message call `ops->activity` first (the idle
  timeout). Gating: `auth_command_rule(protocol, command)`; if `gated`,
  `ops->validate_secret(secret)` else `"Not authenticated"` (never echo the value); then
  unknown (`!known`) → the generic error; then `auth_permits(ops->capabilities_for(c), &rule)`
  else `"Not permitted: <protocol>:<command>"`. `environment:challenge {user, cnonce}` →
  `ops->challenge`: `AUTH_OK` reply `{salt, iterations, nonce}` from the `AuthChallenge`;
  `AUTH_DELAYED` send nothing now — the `on_challenge` callback (from `auth_tick`) sends the
  reply later (its argument NULL: `"Login failed"`); `AUTH_FAILED` `"Login failed"`.
  `environment:login {user, nonce, proof}` → `ops->verify_proof`, then on `AUTH_OK`
  `ops->issue_secret` (false: `"Login failed"`) and reply `{secret, role
  (users_role_name), capabilities (auth_capability_name of the set bits in order),
  server_signature}`; `AUTH_FAILED` `"Login failed"`; `AUTH_FAILED_CLOSE` `"Login failed"`
  then `ws_close(conn, WS_CLOSE_POLICY, ...)` (1008, 02 §2.5). Missing or mistyped
  payload fields of `challenge`/`login` also answer `"Login failed"` (pass NULL). `logout` →
  `ops->revoke_secret`, reply `{"protocol":"environment","command":"logout"}` (no payload).
  `on_expired` (from `auth_tick`): send that same logout message and stop event delivery.
  Event delivery and `runtime:runtime`'s capabilities use `ops->capabilities_for(c)` (0 =
  not authenticated). Callbacks never come from inside an `ops` call, only from
  `auth_tick`, and must not call `ops->close`.
  Done: the dispatcher `src/protocol/dispatcher.h` (`Protocol`, one `ProtoConn` session per
  connection behind a `ProtoTransport` — send, space, pending, close — so tests use a fake;
  `protocol_ws_handler()` in `protocol_ws.c` adapts it to `WsConn`), the wire messages
  `src/protocol/messages.h` (envelope, the two special shapes, errors, the replies, ISO 8601
  times, and `proto_msg_event`, the `GraphEvent` → message mapping), `ws_send_pending` in the
  WebSocket server, and `tests/test_dispatcher.c` (fake connections and a fake `Authorizer`,
  plus a full SCRAM login through the real one). Mapped events: graph created/deleted,
  status, started, stopped (a started graph's `delete_graph`), error. Log mirroring is the
  layer's log sink. Decisions: 02 §1.2 (check order, envelope, the raw text bounded to 256
  bytes, the generic error `"Unknown command: <protocol>:<command>"`, `"Invalid field:
  <name>"`, when `"graph"` is set, out of memory closes with 1011, bursts deferred to
  `on_drained` or `"Reply too large"`), 02 §2.5 (dispatcher), §3.2, §3.5, §3.6, §4.1
  (subscription from login, unscoped), 05 §4 (mirroring).
- [x] **18. `graph:*` and `network:*` commands**: every command and broadcast of 02 §3.3–§3.4,
  `graph:connect` snapshot, scoping, `network:edges` live values (02 §4).
  The snapshot must fit the connection's free outgoing space (`ws_send_space`; `WS_OUT_CAP`,
  64 KiB by default, smaller on the MCU, 02 §0 "Outgoing buffer"): if it does not, wait for
  `on_drained` and generate it then (still atomic). Decide (and record in 02 §4.1.1) the
  answer to a graph whose snapshot exceeds the whole buffer — e.g. a `network:error` instead
  of an endless close/reconnect cycle.
  From task 17 (`src/protocol/dispatcher.c`, `messages.c`): add each command to the
  `commands[]` table (`{protocol, command, handler}`; secret gating and the capability check
  are already done before a handler runs, and a known command without a handler gets the
  generic error); add the `graph:*` events to `proto_msg_event`'s switch (the `GraphEvent` →
  message mapping, built once per event for every subscriber; `GRAPH_STARTED`/`STOPPED`,
  `STATUS`, `CREATED`, `DELETED`, `ERROR` are mapped already). Missing or mistyped payload
  fields: `invalid_field()` (`"Invalid field: <name>"`, with `"graph"` for a valid graph id,
  02 §1.2); refusals via `GraphsMessage` and `reply_error(c, msg.text, known_graph(id))`.
  `graph:connect` re-scopes with `protocol_conn_scope` (a login starts unscoped); the
  snapshot is a burst: reuse the burst helpers (`Burst`, `burst_send`, a new `PENDING_` flag
  for `protocol_conn_drained`; `send_pending` 0 means the buffer is empty, so the answer to a
  snapshot that never fits is chosen there — currently `"Reply too large"` for the other
  bursts). `graph:list`/`network:getstatus` are `graphs_status_all`/`graphs_status`.
  `network:edges` subscriptions must end on logout and expiry too (`cmd_logout`,
  `on_expired`) and on close (`protocol_conn_close`).
  Done: every `graph:*` command of 02 §3.4 and `network:*` command of 02 §3.3 in
  `src/protocol/dispatcher.c` (the group commands of 02 §3.4.2 still answer the generic error,
  open question 4), every graph event in `proto_msg_event` plus the snapshot and
  `network:data` builders in `src/protocol/messages.h` (`proto_msg_graph_clear`, `_addnode`,
  `_addedge`, `_addinitial`, `_addport`, `_data`), the `graph:connect` snapshot as a burst with
  `PENDING_CONNECT` (re-scope at the instant the snapshot is sent; `"Graph too large to send:
  <id>"` when it never fits), `network:edges` as `InputWatch`es per connection (coalesced under
  back-pressure, `PENDING_DATA`), and the users file re-read at every challenge
  (`AuthConfig.load_users`, `users_loader`, read into the borrowed store, now non-const).
  Decisions: 02 §1.1 (getstatus wording), §1.2, §2.2 ("When it is read"), §3.3, §3.4 (request
  handling, `changeedge` metadata `{}`), §4.1, §4.1.1, §4.2; 05 §1.2, §1.4 step 3.
- [x] **19. Engine startup + `run`**: the startup sequence of 05 §1.4 wired end to end. Task 11
  parses `run process|daemon` and its options and applies them to the `Config` (`cli_config`),
  then refuses with "not implemented yet" (`cli_execute`): complete `run` here, including
  `config_generate_engine_id` and `config_ensure_server_secret` (05 §1.1) after the overrides.
  Loop wiring (05 §1.4 steps 2, 8, 9): `loop_init` over the runtime before the IO provider
  starts, `posix_stop_signals_install`, `loop_listen` on `cfg.host` for `port` and `port + 1`
  (a failure refuses startup), `loop_run`, then `loop_deinit` and
  `posix_stop_signals_restore`. The web server (task 13) is `web_server_open_config(&loop,
  &cfg, &srv)` in step 8 (a failure refuses startup); after `loop_run` returns,
  `web_server_close(srv)` (safe after the loop's shutdown closed its listener), then
  `loop_deinit`. The WebSocket server (task 14) likewise: `ws_server_open_config(&loop, &cfg,
  &handler, ctx, &ws)` in step 8 (the protocol layer's `WsHandler`, task 17; a failure,
  including `tls.enabled`, which is refused as not implemented yet, refuses startup);
  `ws_server_close(ws)` after `loop_run`, before `loop_deinit`.
  Authorization (task 16): in step 3, after `config_ensure_server_secret`,
  `users_load_startup(&store, cfg.auth.users_file)` (it never refuses startup; `store` a static
  `UserStore`) and `auth_create` with `{&store, cfg.auth.server_secret,
  cfg.auth.idle_timeout_s, cfg.auth.max_lifetime_s, clock = a two-line wrapper over
  loop_now_ms, random_bytes = pal_random_bytes, load_users = users_loader, load_users_ctx =
  cfg.auth.users_file}` (task 18: the users file is re-read at every challenge, into `store`; a
  failure refuses startup), then
  `loop_add_tick(&loop, auth_tick, auth)`; hand `auth_authorizer(auth)` to the protocol layer;
  after `loop_run`: `ws_server_close`, `loop_remove_tick`, `auth_destroy`. A `--users <file>`
  override already lands in `cfg.auth.users_file`.
  Protocol layer (task 17): after the graphs are loaded and before the listeners open,
  `protocol_create(&(ProtocolConfig){&gs, &gf, &cfg, auth_authorizer(auth)}, &proto)` (needs
  `cfg.engine_id` set, i.e. after `config_generate_engine_id`; it registers the log mirror
  sink when `log.mirror` is on; a failure refuses startup), then `ws_server_open_config(&loop,
  &cfg, protocol_ws_handler(), proto, &ws)`. After `loop_run`: `ws_server_close(ws)`, then
  `protocol_destroy(proto)`, then `loop_remove_tick`/`auth_destroy`.
  Done: the portable startup `src/engine/engine.h` (`engine_start` = steps 1-8 with a copy of
  the `Config` and `EngineParams` — the load mode, the platform's start/stop hooks around the
  loop, a test-only `ephemeral_ports` —, `engine_run` = step 9, `engine_stop` = the shutdown,
  also of a partial startup; `engine_web_port`/`_ws_port`/`_loop`/`_config`), the servers'
  `web_server_open_config_at`/`ws_server_open_config_at` (an explicit port, 0 = ephemeral, for
  that test parameter), `run` in `platform/posix/cli.c` (the POSIX stop signals as the hooks;
  the engine registers the block types, so `cli_main` no longer does for `run`),
  `tests/test_engine.c` (real loopback sockets: `GET /`, the upgrade with the own Origin,
  getruntime before login, a SCRAM login, the `graph:connect` snapshot of a graph started at
  boot, the stop, no descriptor leaked; the startup failures) and `run`'s exit codes in
  `tests/test_cli.c`. Decisions: 05 §1.2 (`run`, exit codes), §1.4 (the implementation
  decisions: the PAL initialized before step 1, `graphs/`/`types/` created or startup refused,
  the `.tmp` cleanup, `run process` refuses a graph file that does not load while `run daemon`
  skips it, the ready line, the shutdown order, nothing saved at shutdown), §2.3; 01 §0.1, §5,
  §6.
- [x] **20. End-to-end protocol tests**: real WebSocket client tests from the 02 message
  catalog, refusal cases, multi-client command → event checks (05 §5).
  From task 18: `tests/test_dispatcher.c` covers every command in-process with the fake
  transport (exact payloads, two editors and a viewer, refusals, scoping, snapshot order,
  placeholders, too-large and deferred snapshots, initial encoding, `network:edges`, the users
  file reload); the end-to-end tests should add what only a real socket shows: the snapshot
  against the real `ws_send_space` with `WS_OUT_CAP` (a graph just under and just over the
  buffer: `"Graph too large to send: <id>"`, connection open), the deferred snapshot after a
  burst that filled the buffer (`on_drained`), `network:data` coalescing while the client does
  not read, and a users-file edit while the engine runs (next login).
  From task 19: start the whole engine in-process as `tests/test_engine.c` does
  (`engine_start` with `EngineParams.ephemeral_ports`, a temporary confdir/basedir/webdir, a
  test user with a low iteration count), drive it with `loop_run_once(engine_loop(e))` while
  the clients use non-blocking sockets, and end with `pal_request_stop`, `engine_run`,
  `engine_stop`; its WebSocket client, SCRAM login and frame helpers can move into a shared
  test header.
  Done: the shared helpers `tests/e2e.{h,c}` (start/stop the engine in a temporary
  directory with test users of every role, connect with the own Origin, the SCRAM login,
  send an envelope or raw frames, receive with a bound on loop iterations, expect a message by
  protocol/command and parsed JSON payload — objects in any order, arrays exact, `'*'` for any
  value —, silence checked with a barrier request, collect up to a barrier, expect a close
  code; `test.h` gains `TEST_SHARED_FAILURES` so the helpers' checks count), `test_engine.c`
  on top of them, and three executables (about 2 s together under ASan):
  `test_e2e_catalog` (every request/response pair of 02 §3 — runtime, network including a
  failed persist, graph including the snapshot and `removenode`'s compound events,
  component, trace, environment —, the 25 refusal cases of §3.4.1 each with the error to the
  sender only and no event on two other connections, the envelope errors of §1 and the raw
  text cut at 256 bytes, the generic error per sub-protocol, `"Not authenticated"` including a
  secret of another connection, `"Not permitted: …"` for the viewer and operator, `"Login
  failed"`, the close with 1008 after three failures, logout), `test_e2e_clients` (the same
  events as byte-identical frames on two editors whoever sends, a conflicting `addnode` won by
  the first, refusals without events, scoping to another graph, an unauthenticated
  connection seeing nothing, the snapshot exactly as large as an empty outgoing buffer and one
  byte larger, the snapshot deferred behind a reply in the buffer, `network:data` coalescing
  while the client does not read, the users file edited while running, a 1009 close (whole
  and fragmented), a command in three fragments across many TCP writes with a ping between,
  200 pipelined commands, clients reset mid-burst in both directions, log mirroring) and
  `test_e2e_dataflow` (three chained `math/Add2` with live values through initials, a stop and
  start and a removed edge; a 20 ms `timing/Interval` toggling on the real clock;
  `network:persist`, a restart in daemon mode and the same snapshot, started, values flowing).
  No engine bug found. Decisions: 05 §5 (implementation note).
  Fix afterwards (found by a probe during task 20): a fast `network:data` stream filled the
  outgoing buffer to within one value of full, so the client's next reply or the next event no
  longer fit and closed the connection (02 §0). Lossy messages (`network:data`, the log
  mirror) now leave a **reserve** free — a quarter of the buffer (`send_capacity /
  PROTO_DATA_RESERVE_DIV`, 16 KiB of the default 64 KiB, a per-build override) — that replies
  and events may use; and while a burst is deferred no lossy message is sent, so a stream
  cannot keep the buffer from draining (the burst goes first at `on_drained`, the coalesced
  values after it). `ProtoTransport` gains `send_capacity` (`ws_send_capacity`). Tests:
  `test_data_reserve` in `test_dispatcher.c` (the boundary at the reserve and one byte into it,
  a reply and an event using the reserve, a value too large for any buffer dropped, the
  burst's precedence) and `test_data_stream_replies` in `test_e2e_clients.c` (a watcher that
  stops reading while values stream sends `network:getstatus` and `graph:changenode` and gets
  an unrelated `runtime:new_graph`: it stays connected and gets all of them, then the current
  value; without the reserve it is closed). Decisions: 02 §0, §1.2, §4.2.

## Phase 4 — Provisional / blocked areas

- [x] **21. Composite block types — runtime + `types/` persistence** (03 §5–§5.1). The
  `graph:*group` wire commands are blocked (open question 4) and still answer the generic
  error: only their model side is done.
  From task 17: `component:getsource` returns `code: ""` for every type (`cmd_getsource` in
  `src/protocol/dispatcher.c`): give a composite its authoring JSON there; the
  `component:component` broadcast of a new or changed type (02 §3.4.2, system-wide per §4.1)
  needs a broadcast to every connection with a live session (as the log mirror iterates
  them), built with `proto_msg_component`.
  From task 19: startup step 6 is a marked no-op in `engine_start` (`src/engine/engine.c`),
  between the built-in block types (step 4) and the graphs (step 7); `<datadir>/types/` exists
  by then (created in step 2) and its `.tmp` leftovers are gone.
  Done: `CompositeDef` in `src/graphs/block_type.h` (resolved inner blocks, edges, initials,
  port mappings; `BlockTypeDef.composite` is typed now); composite instances on the one
  generic runtime (`src/graphs/graph.c`, `block.c`: an instance is an ordinary block whose
  inner blocks are real blocks kept in the instance, `Block.inner`/`parent`; the structure
  stays on the instance, the dataflow maps through `block_input_target`/
  `block_output_source`; `block_input_value`/`block_output_value` read through it, used by
  the dispatcher's coalesced `network:data`); `src/blocktypes/composite_types.h` (parse,
  validate and register the 03 §5.1 JSON with every refusal and its message, the canonical
  document and `composite_types_source`, unregister/rename refused while in use, change
  description/icon, `composite_types_reset`, and `composite_types_assemble`, the 03 §5.2
  assembly from a selection); `src/app/type_files.h` (save, delete, startup loading in
  dependency order; `graph_files_read`/`graph_files_list` shared from `graph_files.c`);
  startup step 6 and the shutdown in `engine.c`; `component:getsource` returns a composite's
  canonical JSON. Tests: `tests/test_composite.c` (refusals, round trip through text and
  files, nesting, instances: values, initials, defaults, stale queued values, watches,
  rename, removal, a `timing/Interval` inside, the threshold override, a graph file with
  an instance, registry operations, loading `types/` with leftovers, the assembly),
  `test_composite` in `test_dispatcher.c` (list, getsource, snapshot, `network:edges`),
  `test_types` in `test_engine.c` (startup step 6 through `engine_start`). Decisions: 03 §5
  (runtime: mapping, inner naming, lifecycle, defaults, change threshold), §5.1 (shape,
  check order and messages, fan-in, canonical form, registry operations, description/icon
  changes allowed while in use), §5.2 (order, default names, what is left out); 01 §2.3,
  §3.3; 02 §3.4.2, §3.5; 05 §1.4 step 6 and shutdown, §2.3 (type files).
  For the group commands (task 21b): `graph:addgroup` is
  `composite_types_assemble` + `composite_types_register` + `type_files_save`;
  `removegroup` `composite_types_unregister` + `type_files_delete`; `renamegroup`
  `composite_types_rename` + `type_files_save` (new) + `type_files_delete` (old);
  `changegroup` `composite_types_change` + `type_files_save`; each new or changed type then
  needs a `component:component` broadcast to every connection with a live session (as the
  log mirror iterates them), built with `proto_msg_component`; the provisional messages of
  03 §5.1 become the `network:error` texts.
- [ ] **21b. Group wire commands** (02 §3.4.2): `graph:addgroup`/`removegroup`/`renamegroup`/
  `changegroup` on the task-21 model functions, with their broadcasts and a `component:component`
  broadcast for new/changed types. Open question 4 is decided: "save as component" — the
  selected nodes are left unchanged.
- [ ] **21c. Drop the change-threshold override; add `process/Deadband`** (open question 7,
  01 §3.3, 03 §3, 02 §3.4, 05 §2.1): remove the per-instance `change_threshold` from node
  metadata handling (validation, merge, composites, snapshots, files — an old key becomes an
  ordinary opaque key) and add the `process/Deadband` block with type default threshold 0.
- [ ] **21d. Remove `Metric`** (open question 9, 01 §2.1): drop the `Metric` value type,
  its parsing (`"<number> <unit>"` now gives 0.0 like any invalid number text), stringify,
  JSON/file forms and the `BlockCtx.error` flag, from the code and the tests.
- [ ] **21e. Host-managed lifetime + local C API** (open question 12, 02 §6, 05 §1.4): the
  engine stops loading/starting graphs itself; the host (POSIX `main.c`, later the Zephyr
  app) does it through a local C API carrying the protocol's commands and events; the POSIX
  host reads `processes.json` (05 §1.2). The API is typed C functions + event structs over
  the same `graphs_*` command layer as the dispatcher (02 §6).
- [ ] **21f. Links between processes** (open question 13, 01 §2.2, 05 §1.2): explicit links
  from one process's outport to another's inport, declared in `processes.json` and created by
  the host through the C API; values travel like edges; stopped source → last value kept,
  absent source or removed link → default. Runtime link commands for the UI (wire + C API)
  and persisting UI-made links are still to be designed.
- [ ] **22. Environment SPI + simulated environment + bindings** (04 §1, §4; 03 §4) — no
  `io/*` blocks (open question 6 decided: outputs keep their last value); the bindings-file
  format waits for open question 3, which is to be settled while implementing this task. IO events reach the engine through `pal_handoff_push` (01 §0.2); dispatch
  `WORK_IO_EVENT` in `runtime_drain`.
  From task 19: startup step 5 is a marked no-op in `engine_start` (`src/engine/engine.c`),
  after `loop_init` (the handoff exists) and the authorizer; the provider would be a platform
  part handed in through `EngineParams` like the start/stop hooks, stopped in `engine_stop`
  before the loop.
- [ ] **23. `process/PID`** — behavior decided (03 §6.1); **waits for the user's PID code**: the
  implementation is to be based on the user's existing implementations, and the user adds the
  parameters `Pconst`, `Itime`, `Dtime`, `MaxD`. Don't start it without that code.
- [ ] **24. Calendar blocks + standard schedule composites** (03 §6.2): the primitives
  (`timing/MinuteRange`, `DayOfWeek`, `MonthOfYear`, …) with selector-string inputs, the
  `logic/*` bool blocks, and the schedule composites as a separate standard library of type
  files (not built in). Needs the wall clock from the environment interface (04 §1); local
  time from the `time_zone` POSIX TZ string in `hakkas.json` (05 §1.1, a config key to add).
- [ ] **25. Zephyr port** (`src/platform/zephyr/` + Zephyr module) — after phase 3. Set a
  smaller `WS_OUT_CAP` (e.g. 16 KiB) for the WebSocket connections' outgoing buffers (02 §0,
  "Outgoing buffer": 64 KiB × `max_connections` is too much SRAM).
  From task 19: the boot calls `engine_start` (`ENGINE_LOAD_ALL`, 05 §1.3), `engine_run`,
  `engine_stop` with a `Config` built from `/lfs/config/hakkas.json`; LittleFS is mounted and
  `pal_init` done before the configuration is read, and networking (which needs the `net.*`
  keys) comes up after it — e.g. in the `platform_start` hook (05 §1.4).
