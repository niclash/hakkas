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
  `graph:*group` wire commands were blocked (open question 4); only their model side is done
  here (the wire commands: task 21b).
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
- [x] **21b. Group wire commands** (02 §3.4.2): `graph:addgroup`/`removegroup`/`renamegroup`/
  `changegroup` on the task-21 model functions, with their broadcasts and a `component:component`
  broadcast for new/changed types. Open question 4 is decided: "save as component" — the
  selected nodes are left unchanged.
  Done: the commands are `type_files_add_group`/`_remove_group`/`_rename_group`/`_change_group`
  in `src/app/type_files.h` (model + type file + events, like `graph_files_persist`), called by
  the dispatcher's `cmd_*group` (field checks); new events `GRAPH_EVENT_GROUP_ADDED/REMOVED/
  RENAMED/CHANGED` (graph-specific) and `GRAPH_EVENT_COMPONENT` (system-wide) through
  `graphs_notify_group`/`graphs_notify_component`, mapped in `proto_msg_event`;
  `composite_types_rename_check` (the rename's checks alone). Decisions in 02 §3.4.2 (scope,
  field order, refusal order, applied event shapes, no `component:*` for a removal, a failed
  save refuses the command and rolls back: `addgroup` unregisters, `renamegroup`/`changegroup`
  write the file before the model changes; a failed deletion is logged and stands), §1.2,
  §3.4, §3.5; 03 §5.1 (the messages are the documented texts), §5.2 (no explicit port
  names); 05 §2.3. Tests: `test_groups`, `test_group_refusals`, `test_group_storage` in
  `test_dispatcher.c` (broadcasts on a, b, v and the system-wide `component:component` on a
  connection scoped to another graph, every refusal to the sender only, the selected nodes
  unchanged, the files written/renamed/deleted, the rollback of a failed save),
  the group events' scoping in `test_graphs.c`, `test_groups` in `test_e2e_catalog.c`.
- [x] **21c. Drop the change-threshold override; add `process/Deadband`** (open question 7,
  01 §3.3, 03 §3, 02 §3.4, 05 §2.1): remove the per-instance `change_threshold` from node
  metadata handling (validation, merge, composites, snapshots, files — an old key becomes an
  ordinary opaque key) and add the `process/Deadband` block with type default threshold 0.
  Done: `BlockMetadata.change_threshold`, `BlockChange.threshold`/`change_threshold` and
  `enum threshold_change` are gone; `block_change_threshold` is type default → engine-wide
  (inner blocks alike); `graph_json` treats `change_threshold` as an opaque key (no
  validation, kept in its order after `x`, `y`). `process/Deadband` in
  `src/blocktypes/process.c` (after `process/Linear`, icon `filter`, no state, type default
  threshold 0). Decisions in 03 §3: START sets `out = in`; a `threshold` change alone
  re-evaluates; a NaN or negative `threshold` counts as 0; STOP changes nothing. Tests:
  `test_deadband`/`test_deadband_zero` and the opaque-key checks in `test_graph.c`,
  `test_graph_json.c`, `test_graphs.c`, `test_composite.c`, `test_graph_files.c`,
  `test_dispatcher.c`; the descriptor in `test_block_types.c`.
- [x] **21d. Remove `Metric`** (open question 9, 01 §2.1): drop the `Metric` value type,
  its parsing (`"<number> <unit>"` now gives 0.0 like any invalid number text), stringify,
  JSON/file forms and the `BlockCtx.error` flag, from the code and the tests.
  Done: `Metric`, `enum metric_category`, `LINKABLE_METRIC` and `linkable_from_metric` are
  gone from `graphs/linkable.h` (the heap strings now back only string values);
  `linkable_parse` of a `"number"` is f64 when wholly a number, else 0.0; `Block.error` and
  `BlockCtx.error` are gone. Tests: `test_exactly_five_types` and the `"4.2 V"` → 0.0 cases
  in `test_linkable.c`, `test_graph_json.c`, `test_graph_files.c` (from a file),
  `test_dispatcher.c` (`graph:addinitial`) and `test_e2e_catalog.c`; the back-pressure
  tests in `test_e2e_clients.c` stream large values into a string port of a test type.
- [x] **21e. Host-managed lifetime + local C API** (open question 12, 02 §6, 05 §1.4): the
  engine stops loading/starting graphs itself; the host (POSIX `main.c`, later the Zephyr
  app) does it through a local C API carrying the protocol's commands and events; the POSIX
  host reads `processes.json` (05 §1.2). The API is typed C functions + event structs over
  the same `graphs_*` command layer as the dispatcher (02 §6).
  Done: the public header `src/hakkas.h` (code `src/engine/hakkas.c`): `hakkas_create`
  (startup steps 1-6, 8 over `engine_start`), `_run`, `_run_once`, `_request_stop`,
  `_destroy`, `_add_tick`; `hakkas_process_load` (stopped, returns the id and the file's
  `started` flag), `_load_saved`, `_create`, `_delete`, `_rename`, `_start`, `_stop`,
  `_persist`, `_status` (a read, no event), `_count`/`_at`; `hakkas_block_*`,
  `_connection_*`, `_initial_*`, `_port_*`, `_group_*` (one per `graph:*` command, metadata
  as JSON object text, typed initial values); `hakkas_subscribe` delivering a `HakkasEvent`
  per `GraphEvent`, unscoped; refusals `HAKKAS_REFUSED` with the wire's messages
  (`"Invalid field: <wire field>"` for a NULL argument); model-changing commands from an
  event callback are `HAKKAS_ERR_BUSY`. `engine_start` no longer has step 7 (no
  `EngineParams.load`/`graph_files`; `engine_graphs`/`engine_files` accessors);
  `graph_files_load_text`/`_load` never start a graph and report the flag (`bool *started`),
  `graph_files_load_all` takes a per-graph callback, `graph_files_load_list` is gone (the
  host splits the list). The POSIX host `src/platform/posix/host.{h,c}`: `run process`,
  the new `run processes <file>` and `run daemon` load through the C API, then start in load
  order the processes whose list entry has `"start": true`, or without one whose file's
  flag is set; lifecycle events logged at info. The process list parser
  `src/app/process_list.{h,c}` (portable): both forms, paths relative to the list's
  directory, strict validation, unknown members warned; `bindings` and `links` logged at
  info and ignored. `describe topology` starts a graph whose flag is set, so it still prints
  the canonical form. Decisions: 02 §6 (header, API shape, results, events, threading,
  time), 05 §1.2 (`run processes`, process lifetime in the POSIX host, the process list
  file), §1.3, §1.4 step 7 and its failures, §2.1, §2.2, §2.3; 01 §5, §6; 00-README open
  questions 12, 13. Tests: `test_api` (every function's success and refusals with the exact
  messages, the typed events rendered from their fields equal the frames a logged-in
  WebSocket client receives, for commands through the C API and over the wire; busy
  callbacks, subscribing from a callback, a tick hook, load/load_saved/persist/delete with
  files), `test_process_list`, the host's modes in `test_cli` (`run process`, `run daemon`,
  `run processes` in both forms with start flags, relative paths, a missing or malformed
  list, a graph that does not load, `examples/example.json` through a list, the
  bindings/links log lines), `test_engine` (the engine starts no process by itself; the e2e
  helpers load processes through the C API), `test_graph_files` (the flag reported, nothing
  started).
- [x] **21f. Links between processes** (open question 13, 01 §2.2, 05 §1.2): explicit links
  from one process's outport to another's inport, declared in `processes.json` and created by
  the host through the C API; values travel like edges; stopped source → last value kept,
  absent source or removed link → default. Runtime link commands for the UI (wire + C API)
  and persisting UI-made links are still to be designed.
  From task 21e: `app/process_list.h` already parses `links` (an array of objects with
  non-empty strings `from`, `to`; only the count is kept, `ProcessList.link_count`) and the
  POSIX host (`platform/posix/host.c` `load_list`) logs `<file>: <n> link(s) ignored: links
  between processes are not implemented yet` — keep the links (split `from`/`to` on the last
  `.`, 01 §2.4) and create them through new C API functions after every process is loaded
  and before any is started (the host loads all first, then starts). Links in `run process`
  / `run daemon` mode have no source yet. The C API's events would need a link event type
  (`HakkasEvent`), and 05 §1.2's "Not implemented yet" bullet and 00-README open question 13's
  last sentence change.
  Done: the model in `src/graphs/graph_links.c` (declared in `graphs/graph.h` "Links between
  processes"): a `ProcessLink` (source graph + outport name, target graph + inport name, the
  resolved inner block/port of each) kept in the `Runtime` (`rt->links`, creation order), an
  entry of the outport's inner output's fan-out `Link`s and the inport's input's third kind of
  source, `INPUT_SOURCE_LINK` (edge machinery reused: `block_links_output`,
  `output_links_reserve`, `output_unlink`, `input_source_changed`, `input_clear_source`,
  `input_revert` are now shared in `graph_internal.h`). `graph_link_add`/`_remove`,
  `graph_links_remove_graph` (deletion), and internally `graph_links_remove_port` (port
  removal/rename, so also block removal) and `graph_links_drop_graph` (silent, in
  `graph_destroy`). `graph_add_initial` refuses a linked input; `graph_remove_block`'s
  outbound pass leaves links to the ports. Registry: `graphs_add_link`/`graphs_remove_link`,
  link events system-wide, `graphs_delete_graph` removes the links after the stop and before
  `GRAPH_DELETED`; new statuses `GRAPH_ERR_INVALID_LINK_DIRECTION`, `_LINK_EXISTS`,
  `_NO_SUCH_LINK`. Events `GRAPH_EVENT_LINK_ADDED`/`_REMOVED` (graph = the source, `.link`);
  `proto_msg_event` returns NULL for them (no wire message). C API: `hakkas_link_add`,
  `_remove`, `hakkas_link_count`/`_at` (`HakkasLink`), `HAKKAS_EVENT_LINK_ADDED`/`_REMOVED`
  with `to_process`/`to_port`. Process list: `ProcessList.links` (`ProcessListLink`, each
  reference split on its last `.`; a reference without a process or port is malformed).
  POSIX host: links made after loading and before starting, a refused link refuses startup,
  `link … made`/`removed` logged at info; the "ignored" log line is gone. Decisions: a linked
  port removed or renamed (also with its block) removes the link (reported before the port's
  event), a process may link to itself, block/process renames keep links, a link onto an input
  with a retained connection is refused, link events have no wire message (01 §2.2 "Links
  between processes", §4; 02 §6 "Links between processes", events, threading; 05 §1.2 links,
  monitoring, validation, "Not implemented yet"; 05 §2.1 inports/outports; 00-README open
  question 13). Tests: `test_links` (new: values and threshold, delivery on link, stopped
  source, removal with queued values dropped, deleting either process, port removal/rename and
  block removal with event order, renames, self-link, fan-in both ways incl. a retained
  connection, refusals, links left at shutdown), `test_api` (`test_links`: messages, host
  events, the wire client sees exactly the non-link events, values watched over the wire;
  `test_links_from_files`: two graph files chained by a process list's link),
  `test_process_list` (links split, malformed references), `test_cli` (`run processes` with a
  link: made before the starts; an unknown port or process refuses startup).
  **Still open** (open question 13): runtime link commands for the UI — wire commands and
  messages (`graph:*`-like or `runtime:*`), whether links appear in the `graph:connect`
  snapshot or `network:status`, their capability (02 §2.4) — and persisting a link made at
  runtime (UI or C API) into the host's `processes.json` (or another deployment file); until
  then a runtime link lives until shutdown and `run processes` recreates only the list's.
- [x] **21g. Hakkas terms in files + sister-project compatibility** (00-README Terminology,
  05 §2.1, 03 §5.1): graph files write `{"block","port"}` (connections and graph ports) and
  composite type files `"connections"`; the loaders also accept `"node"`, `"process"` (the
  sister project's `examples/example.json`) and `"edges"`, and ignore an extra `"data"` in a
  connection's `tgt`. Test with the files in `examples/`. Non-wire C identifiers may follow
  the same terms (process/block/connection) where that improves clarity.
  Done: `src/app/graph_files.c` saves `"block"` and reads any one of `block`/`node`/`process`
  (more than one: the entry is skipped as invalid, 05 §2.1); `src/blocktypes/composite_types.c`
  saves `"connections"` (also `component:getsource` and the assembled group) and reads
  `"edges"` when it is alone (both: `"Invalid field: edges"`, 03 §5.1). The wire is unchanged.
  `test_graph_files` loads `examples/example.json` and `empty.json` from the repository
  (`HAKKAS_EXAMPLES_DIR`) and checks their canonical save.
- [x] **22. Environment SPI + bindings + the POSIX socket adapter** (designed together with the
  user: 04 §0, §0.1, §1, §4; 05 §1.1; 00-README open questions 2, 3, 6, 11). Implement: the
  SPI types (`HakkasEnvironment`, the description with modules/metadata and addressable
  ports with `count`, the sink, values) handed to `hakkas_create`; applying bindings files
  (format and error rules of 04 §4) from `processes.json`'s `bindings` members — sensors as
  the bound inputs' one source with the engine-wide threshold, actuators written on every
  propagation of the bound output, one driver per actuator channel, missing channels skipped
  with a warning; the current-values-first rule; dispatching `WORK_IO_EVENT`; the wall clock
  from the SPI (replacing the interim `pal_wall_ms`); the POSIX adapter for the external
  environment program (Unix socket client at `environment.socket`, newline-delimited JSON,
  waiting for the description at startup, last values kept and automatic reconnect,
  identical-description check, actuator re-send). Tests use a fake environment server inside
  the test suite. Not in scope: the Colibri simulator (a separate project, later), the
  frontend/wire representation of the environment (deferred), the Zephyr firmware side.
  Earlier notes:
  `WORK_IO_EVENT` in `runtime_drain`.
  From task 19: startup step 5 is a marked no-op in `engine_start` (`src/engine/engine.c`),
  after `loop_init` (the handoff exists) and the authorizer; the provider would be a platform
  part handed in through `EngineParams` like the start/stop hooks, stopped in `engine_stop`
  before the loop. From task 21e: hosts create the engine through `hakkas_create`
  (`src/hakkas.h`), so the provider also needs a field in `HakkasParams`, copied into
  `EngineParams`. The process list's `bindings` member is parsed and resolved relative to the
  list's directory (`ProcessListEntry.bindings`, `app/process_list.h`) and the POSIX host logs
  `<graph>: bindings <path> ignored: environment bindings are not implemented yet` (`load_list`
  in `platform/posix/host.c`): apply the bindings file there, after the process is loaded and
  before it is started; update 05 §1.2's "Not implemented yet" bullet.
  From task 24: the wall clock is still the interim `pal_wall_ms()` (`platform/pal.h`), read by
  `loop_run_once` (`engine/loop.c`), `engine_start` and the C API's `enter`
  (`engine/hakkas.c`) into `runtime_set_wall_ms`; move it into the environment interface
  (04 §1: the environment provides the wall clock), drop `pal_wall_ms`, and switch the wire
  timestamps (02 §3.3) from uptime to it. Keep the runtime's injected wall clock and its step
  handling (01 §3.4).
  Done: the public SPI `src/hakkas_env.h` (`HakkasEnvironment` with `describe`, `start`,
  `stop`, `write`, `wall_ms` — `NULL` falls back to `pal_wall_ms`, which stays as that
  fallback — and `ctx`; `HakkasEnvDescription`/`HakkasEnvPort`/`HakkasEnvModule`; the opaque
  sink with `hakkas_env_sink_push` for changes from any thread and `hakkas_env_sink_current`
  for the current values within `start`, decided so they are there before bindings apply and
  processes start and cannot overflow the handoff), handed in by `HakkasParams.environment`.
  `src/drivers/`: `env_description.{h,c}` (validation: unique ids across both directions,
  counts 1–1024, types, min ≤ max, module port ids, limits; the JSON form; equality),
  `bindings_file.{h,c}` (the 04 §4 file, unknown members warned), `environment.{h,c}` (startup
  step 5; the sink; per-channel conversion and engine-wide threshold; actuator writes converted
  to the port's type; the wall clock; `environment_bind_all`, all or nothing with the skips).
  `src/graphs/graph_bindings.c`: the binding model as a fourth input source kind
  (`INPUT_SOURCE_ENV`, fan-in refused both ways) and actuator-marked outputs written on every
  push incl. the start push; bindings end with their block or process, a removed sensor binding
  reverts its input (severing), actuators keep their values; `GRAPH_EVENT_BINDING_*`, no wire
  form. Work queue IO events carry (sensor port, channel); `runtime_drain` dispatches them.
  Wire timestamps are the wall clock (`Graph.wall_started`, `GraphEvent.wall_ms`); the loop
  reads the environment's clock (`loop_set_wall_clock`). C API: `hakkas_bindings_apply`,
  `hakkas_binding_add`/`_remove`/`_count`/`_at`, `hakkas_environment`, BINDING events,
  `wall_ms` on events and `wall_started_ms` in the status. Config key `environment.socket`.
  POSIX: `platform/posix/env_link.{h,c}` — the Unix-socket client and its message set
  (hello/write; describe/value/clock), the startup wait for the description and every
  current value (stop request honoured), one link thread with reconnect, the
  identical-description check, actuator re-send, per-channel coalescing (no overflow drops),
  queued log messages logged by a tick hook; the host applies a process list's bindings right
  after each process loads (the "ignored" log path removed). Tests: `test_environment`
  (description, JSON, bindings file, sensor/actuator/fan-in/threshold/stopped-started rules,
  every bindings-file refusal and skip, ending bindings, the wall clock), `test_env_api`
  (startup and shutdown, a sink thread, the environment's wall clock on the wire and the
  fallback, the C API's bindings), `test_env_link` (a fake environment program in the test:
  startup wait, values, writes, clock, invalid messages, disconnect and reconnect with re-send,
  changed description, startup failures), `test_cli` (`run processes` with bindings end to
  end). Specs: 04 §0 (C types, rules), §0.1 (message set, startup, reconnect, overflow
  policy), §1 (wall clock), §4 (file form, applying, ending a binding); 01 §0, §2.2, §3.2,
  §3.2.1, §3.4, §6; 02 §3.3, §6; 03 §4; 05 §1.1, §1.2, §1.4; 00-README questions 2, 3, 11.
  Revised 2026-10-13 (user decision): hakkas no longer waits for sensor values at startup and
  has no timeout for them — only the description is awaited. Processes start from their
  inputs' defaults (the start push drives the actuators from them) and real values propagate
  as they arrive; start-up protection of the plant is the environment's job (on Colibri the
  Zephyr driver, which also reports a value for every channel, dead sensors included).
  `env_link.c` dropped the per-channel wait and its "waiting for the current values" log;
  values read together with the description are still handed over at `start`.
  `hakkas_env_sink_current` stays as an **optional** SPI call for values an environment
  already has at `start` (taken at once, no handoff overflow; useful for the Zephyr
  firmware), nothing requires it. Test: `test_env_link` `test_no_values` (start from
  defaults, later values propagate). Specs: 04 §0 rule 2, C types, §0.1 messages, startup,
  implementation; 05 §1.1, §1.2, §1.4 step 5; README.
- [x] **23. `process/PID`** — implement the built-in PID of 03 §6.1 (revised 2026-10-13: parallel
  `Kp`/`Ki`/`Kd`, windup protection, auto/manual, max D gain, output min/max). The user may
  refine it later with their own PID code.
  Done: `process/PID` in `blocktypes/process.c`, a `TIMER_PERIODIC` + `compute_on_input` type
  with per-instance state (integral term, derivative term, last measurement, last step time),
  registered after `process/Deadband`. Parallel form with the stored integral term, conditional
  integration that lands the output on a limit and never beyond, the max-D-gain filter on the
  measurement by backward Euler (`D_k = (Tf·D_{k−1} + Kd·Δy)/(Tf + dt)`, `Tf = |Kd|/max_dgain`),
  `dt` from engine time, bumpless manual → auto, non-finite inputs hold the output. A periodic
  type's input drives its period through the new `BlockCtx.period_ms` (applied by the runtime
  like `block_set_period`). Tests: `test_pid` (P step, integral per `dt`, closed loop on a
  first-order plant with and without the integral, derivative filtered/unfiltered and no
  setpoint kick, reverse acting, anti-windup at both limits, limits incl. NaN and swapped,
  manual and bumpless transfer, start in manual, stop/restart, `sample_ms` re-arm and idle,
  skipped ticks, NaN inputs), `test_block_types` (descriptor). Specs: 03 §3 table, §6 intro,
  §6.1 (port table, decisions); 01 §2.3 (`BlockCtx.period_ms`), §3.2 (periodic timers).
- [ ] **25. Zephyr port** (`src/platform/zephyr/` + Zephyr module) — after phase 3. Set a
  smaller `WS_OUT_CAP` (e.g. 16 KiB) for the WebSocket connections' outgoing buffers (02 §0,
  "Outgoing buffer": 64 KiB × `max_connections` is too much SRAM).
  From task 19: the boot builds a `Config` from `/carrier/config/hakkas.json`; the LittleFS
  volumes `/mcu` and `/carrier` (two W25Q128s; the file locations of open question 14,
  05 §1.3) are mounted and `pal_init` done before the configuration is read, and networking (which needs the
  `net.*` keys) comes up after it — e.g. in the `platform_start` hook (05 §1.4). From task
  21e: the Zephyr application is the host and uses the local C API (`src/hakkas.h`, 02 §6):
  `hakkas_create(&cfg, &params, &h)`, then its processes — by default like `run daemon`
  (05 §1.3): `hakkas_process_load_saved` with a callback that starts each process whose
  `started` flag is set (`platform/posix/host.c` `on_saved` does the same) — or a process list
  (`app/process_list.h` is portable), then `hakkas_run`, `hakkas_destroy`. The engine itself
  loads no process.

## Phase 5 — Frontend (06)

The browser editor, feature by feature. Rules for every task: a user action only sends a
command and the view changes only from the engine's events (02 §1.1, 06 §3); a refusal shows
the engine's `network:error` and changes nothing; every task adds unit tests where there is
logic and end-to-end tests (06 §6) for what the user does, and records its decisions in 06.
**The wire protocol is not changed without asking** (00-README "Protocol compatibility
policy"): a task that needs an addition stops at the decision below, one addition at a time.

Decisions (made with the user 2026-10-14; recorded in 06 §7, 02 §2.3.1 and §3.3, 03 §3):
- **D1. Saving**: a Save button and an "unsaved" mark; a discreet notice on success, a prominent
  error on failure. Protocol addition: `network:status` gains `unsaved` (02 §3.3) → tasks 45, 32.
- **D2. Starting at boot** is the saved `started` state, which the graph file already holds
  (05 §2.1): start/stop makes a process unsaved, saving stores it. The process list keeps
  showing only loaded processes. No protocol change → task 32 (task 33 merged into it).
- **D3. A process's description** stays read-only for now.
- **D4. Composite types** are view-only for now; editing a type's inside is wanted later and
  needs its own design → task 36.
- **D5.** The bitwise `math/And2`, `Or2`, `Xor`, `And4`, `Or4` are removed; the comparisons
  become `compare/*`, the old names load as aliases → task 47 (task 44 merged into it).
- **D6. Fewer logins**: a reload resumes the session with a resume key, never sent on the wire;
  kept in `localStorage`. Protocol addition (02 §2.3.1) → tasks 46, 39.

### 5.0 Engine prerequisites (C, with their tests and the spec)

- [x] **45. Unsaved state** (02 §3.3 "Unsaved state"): a per-process flag kept by the Graphs
  registry (`graphs/graphs.h`): structural changes since the last load or save, or a started
  state that differs from the file's flag (compared, so a process loaded saved-running and
  started by the host is saved); `network:status` carries it as its last member, broadcast
  whenever it changes and after every save and load; the C API's status event and
  `HakkasProcessStatus` carry it (02 §6). Tests: every kind of change, start/stop round trips,
  load, save, a failed save, a new process, and a restart that keeps a running process saved.
  Also fixed: `test_cli`'s `run process` step used the default port 8080 and failed whenever
  another engine was running there.
- [x] **46. Resuming a session** (02 §2.3.1): the resume table in the authorizer (bounded, LRU,
  checked against the users file at the challenge), the `resume` member of the login reply, the
  `resume` variants of `environment:challenge`/`login`, rotation, expiry with the chain's idle
  timeout and lifetime, revocation at logout, failures counted for the rate limits; logging
  never shows an id or key (02 §2.5 item 8). Then the frontend: keep `{user, id, ResumeKey}` in
  `localStorage`, resume on load before showing the login, forget it at logout or when a resume
  fails. Unit tests (the derivation against an independent implementation, 06 §6), protocol
  tests, and an end-to-end test that a reload stays logged in.
  Decisions made while implementing it are in 02 §2.3.1 (the chain keeps one key; a failed
  resume does not end the entry) and 06 §2 (a failed resume shows the login form without an
  error). The theme end-to-end test now logs out before its reload, which would resume.
- [x] **47. Block types** (03 §3): remove `math/And2`, `Or2`, `Xor`, `And4`, `Or4`; rename the
  comparisons to `compare/*` with the `math/*` names accepted as aliases by the graph and type
  file loaders (a save writes the new name); update 03's table, `describe`, the tests, and the
  palette's end-to-end expectations.
  Done as `src/blocktypes/compare.c` and `block_types_alias`, used by the graph file and type
  file loaders only (so `describe topology` prints the new names; `describe type` and commands
  take the current names). A graph file's bitwise block becomes a placeholder; a composite type
  using one does not load.

### 5a. Working with blocks

- [x] **26. Selection** (06 §4): select one block by click, more with Ctrl/Shift-click or the
  marquee (Rete's selector is already set up), clear with Escape or a click on the canvas,
  select all with Ctrl+A. Delete/Backspace removes the selection (`removenode` per block,
  connections go with them). Moving a selection moves every selected block: the editor blocks
  local moves and sends one `changenode` per pointer movement for the dragged block, so the
  other selected blocks probably stay put (to be checked first) — send each selected block's
  new position (throttled to one
  command per animation frame and per block, the final positions on release; 06 §7). The
  selection is local view state (not engine state); blocks removed by an event leave it.
  Checked first: the other selected blocks did move, but locally — Rete's `selectableNodes`
  moved them when the dragged block's event came back, without commands. It is replaced by
  `floweditor/selection/` (`blocks.ts`, unit tested; `editor.ts`, the wiring), 06 §4.0. The
  marquee is a Shift/Ctrl-drag on the canvas (a plain drag pans) and adds to the selection;
  Ctrl/Shift-click on a selected block unselects it.
- [x] **27. Inspector panel**: a panel beside the editor for the selected block — its id, its
  type and the type's description, and every port with its type and description (from
  `component:component`); with several blocks selected, their count and the common actions.
  The base for tasks 28, 29 and 34.
  Done as `Inspector.vue` beside the editor (06 §4.2), from `inspector.ts` (unit tested) and the
  selection published as `vueModel.selectedBlocks`. Inputs first, then outputs, with each
  port's default; the common action for now is Delete.
- [x] **28. Renaming blocks**: in the inspector, with F2 and from the context menu;
  `graph:renamenode`, the new name shown when its event arrives; `"Node exists"`/`"Invalid id"`
  refusals shown at the field. Blocks added from the palette keep their `<Type>_<n>` names.
  Done in the inspector's name field (06 §4.2): Enter or leaving the field sends; the menu item
  is added to Rete's classic menu for now (task 30 replaces it). A refusal is matched to the
  pending rename by its message (`renaming.ts`), since `network:error` names no command; refusals
  go through `errorClaims` in the model before the error bar.
- [x] **29. Fixed values on inputs** (initials, 02 §3.4): in the inspector, each input shows
  whether it has a connection, a fixed value or its default; a fixed value is set with an
  editor fitting the port's type (a number field, a true/false switch, a text field) →
  `graph:addinitial`, cleared → `graph:removeinitial`. An input with a connection cannot take
  one (the engine refuses: no fan-in) — shown as such. Also from the context menu on an input.
  Done in the inspector (06 §4.2, `InputValue.vue`, `initials.ts`). Numbers are checked in the
  browser (the engine would read other text as 0). The context menu on an input moves to task
  30 (Rete's classic menu does not know which input was clicked). The engine sends no port
  `default` in `component:component`; decided with the user afterwards (2026-10-14): there are
  no per-port defaults, an absent value rests at 0/false/"" (03 §1), so the inspector shows
  that; `timing/Interval` and `process/PID` were changed to rest at those values.
  Then, at the user's request, fixed values drawn as value nodes (06 §4.3, `values.ts`,
  `ValueNodeView.vue`, `ValueDraft.vue`): a line dropped from a free input asks for a value; a
  double-click changes it; pulling the line off clears it. Placed beside the input, with no
  protocol change (the user's choice over upstream's `metadata` on `addinitial`).
- [x] **30. Context menus** (themed, 06 §4.1): replace the classic preset's menu by our own
  component. On the canvas: add block (with search, placed where the menu was opened), paste,
  zoom to fit, auto-layout (sends a `changenode` per moved block). On a block: rename, set
  inputs, watch values, duplicate, copy, delete, "create composite" with a selection. On a
  connection: watch value, delete. On a port: expose as process port (task 35). Keyboard
  accessible, closes on Escape.
  Done as `ContextMenu.vue` and `menus.ts` (06 §4.4), with the input and value-node items of task
  29 (06 §4.3) and "Edit inputs" for "set inputs". Watch values, duplicate, copy, paste, create
  composite and expose as process port join the menus with tasks 34–37. Also: a right-click no
  longer inserts a reroute pin (only main-button presses reach the reroute plugin), and the
  classic context menu plugin is unused (removed with the 3D template in task 43).
- [x] **31. Palette and sockets**: a search field over the block types; drag a type onto the
  canvas to add it at the drop point (today it lands at 100,100); tooltips with the type's
  description and ports; sockets colored by port type (number, bool, string) with a legend in
  the themes (new `--hk-*` variables in all three).
  Done (06 §4.5, `palette.ts`, `SocketView.vue`): one Rete socket per port type; a click now adds
  in the middle of the view; the search matches as the menu's Add block… does.

### 5b. Processes

- [x] **32. Process management** (02 §3.2–§3.4): rename (`graph:rename`, in place in the list),
  start/stop (`network:start`/`stop`, the running mark from the events; a start or stop makes
  the process unsaved, and saving stores whether it starts at boot, D2), save (a Save button
  and an "unsaved" mark from `network:status`'s `unsaved`, after task 45; a discreet notice on
  success, a prominent error on failure, D1), delete (`runtime:delete_graph`, with a confirmation
  naming the process), create (existing "New...": open the new process when its
  `runtime:new_graph` arrives from this client's request). The description shown, read-only
  (D3). Uptime from `network:status`.
  Done (06 §4.6, `Processes.vue`, `ProcessItem.vue`, `processes.ts`). No protocol change: start,
  stop and rename are followed by `network:getstatus`, since their own events reach only the
  process's subscribers. The e2e fixture gained `dataDir` and `allowedErrors` (a failed save).
- [x] **33. Startup activation** — merged into task 32 (D2: the saved `started` state).
- [x] **34. Live values** (`network:edges`/`network:data`, 02 §4.2): watch a connection (context
  menu, inspector, or a "watch all" toggle for the open process) and show its value as a label
  on the connection; watch a block's inputs, including unconnected ones with a fixed value.
  The client keeps the set of watches and sends it whole (a request replaces the previous set);
  the set is cleared when another process is opened and resent after a reconnect. Values as
  the engine sends them (strings), formatted by the port's type; a stale mark when the
  connection is lost.
  Done (06 §4.7, `watches.ts`, `floweditor/live.ts`, `ConnectionView.vue`, which now draws
  connections and value lines alike). The stale mark is set on a lost connection but is seen
  only once task 39 keeps the view while reconnecting.
- [x] **35. Process ports** (`graph:addinport`/`addoutport`/`remove…`/`rename…`, 02 §3.4): a
  process's inputs and outputs, through which processes are linked (01 §2.2): expose a block's
  input or output as a process port (context menu on the port, a name dialog), draw them as
  markers at the canvas edge connected to their block port, rename and remove them. The links
  themselves have no wire messages yet (00-README open question 13), so they are not shown.
  Done (06 §4.8, `ports.ts`, `port-layout.ts`, `PortNodeView.vue`). The client's port event
  types named the broadcast's `name` `public`; fixed.
- [x] **36. Composite types** (02 §3.4.2, 03 §5; "save as component", open question 4): create
  one from the selection (name, description, icon) → `graph:addgroup`; the palette lists
  composites in their category with a mark; change description/icon (`changegroup`), rename
  and remove (`renamegroup`/`removegroup`; refused while in use — shown); view the inside of a
  type read-only, drawn from `component:getsource`. Editing a type's inside is later (D4). Replacing the selection by
  an instance of the new type is a separate, later option (open question 4, "collapse").
  Done, revised with the user (2026-10-14): a type is defined as a **component graph**
  (upstream's `"main": false`; 02 §3.4.3) — engine: `graph:clear`, persist into the type,
  delete, one built per type file at startup, the group commands kept in step; frontend (06
  §4.9): *Create composite type…* with a port mapping dialog (crossing connections ticked,
  any block port can be ticked and named), opening a type from the palette, Save type, Delete
  type. Not yet: renaming a type or changing its description from the frontend; a type file's
  explicit port descriptions are not kept when the type is saved from its graph.
- [x] **37. Copy, paste, duplicate**: the selection with its inner connections and fixed values
  as a clipboard (in the page; also to the system clipboard as JSON), pasted into the same or
  another process as `addnode` + `addinitial` + `addedge` commands, with new names where
  taken and an offset position; each part appears as its event arrives.
  Done (06 §4.10, `clipboard.ts`, `floweditor/copypaste.ts`): Ctrl+C/V through the browser's
  copy/paste events (the system clipboard also on plain http), Ctrl+D and the menus' Copy,
  Duplicate and Paste here; the pasted blocks are selected as they arrive.

### 5c. The application

- [x] **38. Roles** (02 §2.4): from the login's capabilities — a viewer gets a read-only editor
  (no palette, menus without edit actions, no dragging), an operator also start/stop; the
  engine's refusals stay the guard, the UI only stops offering what would be refused.
  Done (06 §4.11, `roles.ts`). Also fixed: an input's pending value command was dropped when
  a connection reached the input meanwhile, and leaving the field sent it again. The two
  browser tests that provoked refusals as a viewer now use an editor (a clip of an unknown
  type; a command held back while another client connects the input).
- [x] **39. Connection resilience**: reconnect after a lost connection with back-off, resuming
  the session (task 46) rather than asking for the password, then reopen the process that was open and resend the watches (task 34).
  Dragging throttle for slow links if task 26 has not covered it.
  Done (06 §2): the editor stays read-only with stale values while it reconnects (1–30 s
  back-off), resuming the session; it is then rebuilt and reopens the process with its watches.
  The dragging throttle was task 26's. The e2e fixture gained `restart()` (same ports).
- [x] **40. Addresses**: the open process in the URL (`/process/<id>`; the web server's SPA
  fallback serves it, 05 §3), so a process can be bookmarked and the browser's back button
  works; after login the URL's process opens.
  Done (06 §2.1, `src/address.ts`) as `/?process=<id>` and `/?type=<name>`: in the query,
  since the web server takes a path's `%2F` and `.` as a file's and graph ids may hold both.
- [x] **41. Messages**: the log mirror (`network:output`/`network:error` from `log.mirror`,
  05 §4) in a collapsible panel; errors with their process named; the user name and role in
  the top bar; the runtime's label and version (`runtime:runtime`) in an "about" view.
  Done (06 §4.12, `messages.ts`, `MessagesPanel.vue`, `AboutDialog.vue`); the login event now
  carries the user name.
- [x] **42. Undo/redo** — later: the client's own commands, undone by their inverse commands
  (`removenode` for `addnode`, …) sent to the engine like any other, so other users see them
  too; an undo whose state was changed by someone else meanwhile is refused or skipped.
  Done (06 §4.13, `undo.ts`, `floweditor/history.ts`; decided with the user: sessionStorage,
  500 steps per process, a step changed by someone else skipped with a message). Ctrl+Z,
  Ctrl+Shift+Z / Ctrl+Y; steps checked before being undone or redone.
- [x] **43. A smaller bundle** (06 §5): drop the unused 3D editor template (three.js) and the
  icon fonts nothing uses, build `.gz` assets only for the device, and record the size
  against the flash budget.
  Done (06 §5): the templates, three.js, the unused Rete plugins, the icon fonts and Sass gone;
  elkjs loaded only for Arrange blocks; `npm run build:device` writes `dist-device`, gzip
  only, 518 KiB (about 3 % of the 16 MB flash). The whole browser suite passes against it.
- [x] **44. Block categories** — merged into task 47 (D5); the palette's sections follow the
  type names.

### 5d. Waiting for other decisions

- **Links between processes in the UI**: needs the wire messages of 00-README open question 13.
- **Environment ports and bindings in the UI**: deferred (03 §4, 04 §1); the UI should let users
  hide and organize environment ports.
- **Connection routes**: Rete's reroute pins kept in the connection's metadata (`changeedge`,
  `route`), so a hand-routed wire survives a reload and is shared with other users.

## Phase 6 — More block types (03 §3.2–§3.9, §7.1)

Decided with the user 2026-10-15 and specified first; each type is one commit, in the order of
the 03 §3 table, in `src/blocktypes/process_more.c` (registered after `process/PID`), with its
behavior tested in `tests/test_process_more.c`.

- [x] **48. `process/Curve`** (03 §3.2): seven points, the non-rising ones skipped, linear
  interpolation, `min`/`max` outside the end points.
- [x] **49. `process/AutoMan`** (03 §3.3).
- [x] **50. `process/Alarm`** (03 §7.1): the transition table, `disable` over `block`, leaving
  them and `START` through `Normal`; `category` and `class` are for alarm views (none yet).
- [x] **51. `process/Delay`** (03 §3.4).
- [x] **52. `process/Trigger`** (03 §3.5).
- [x] **53. `process/Limit`** (03 §3.6).
- [x] **54. `process/Hysteresis`** (03 §3.7).
- [x] **55. `process/Exponential` and `process/Logarithmic`** (03 §3.8).
- [ ] **56. `process/Integrator`** (03 §3.9).
