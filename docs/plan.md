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
- [ ] **13. HTTP static server** (05 §3): GET only, SPA fallback, `.gz` variants. Build it on
  the loop (`engine/loop.h`): `loop_listen` with an `on_accept` that sets a `LoopConnHandler`;
  stream files through `on_drained`/`loop_conn_space`, pause reading with
  `loop_conn_pause_read` while a response is pending, `loop_conn_close` after the response;
  choose the listener's `out_cap` and a request-header limit, and an idle timeout via a tick
  hook (`loop_add_tick`) if one is wanted.
- [ ] **14. WebSocket layer** (RFC 6455): upgrade, Origin check, connection and frame-size
  limits (02 §0). On the loop: `max_connections` is counted with `loop_listener_conn_count`
  (refuse the upgrade with 503 above it; set the listener's hard `max_conns` somewhat higher
  so the 503 can be sent); choose `out_cap` (a full outgoing buffer closes the connection,
  01 §3.2.1).
- [ ] **15. Crypto primitives**: SHA-256, HMAC-SHA256, PBKDF2, base64/base64url, constant-time
  compare, via mbedTLS (decided, 01 §6).
- [ ] **16. Authorization**: users file, SCRAM handshake, sessions, roles/capabilities, rate
  limiting, `hakkas user add|passwd|del|list` (02 §2). The CLI (task 11, `platform/posix/cli.c`)
  parses `user <add|passwd|del|list>` and refuses it as "not implemented yet": implement the
  subcommands and their arguments here. The users file path is `Config.auth.users_file`.
  Session expiry and login back-off are deadlines: run them from a tick hook
  (`loop_add_tick`, 01 §3.2.1); rate limiting keys on `loop_conn_peer(conn)->addr`.
- [ ] **17. Protocol dispatcher**: envelope validation, error format, secret gating,
  capability checks, `environment`/`runtime`/`component`/`trace` (02 §1, §3.1–§3.2, §3.5–§3.6).
- [ ] **18. `graph:*` and `network:*` commands**: every command and broadcast of 02 §3.3–§3.4,
  `graph:connect` snapshot, scoping, `network:edges` live values (02 §4).
- [ ] **19. Engine startup + `run`**: the startup sequence of 05 §1.4 wired end to end. Task 11
  parses `run process|daemon` and its options and applies them to the `Config` (`cli_config`),
  then refuses with "not implemented yet" (`cli_execute`): complete `run` here, including
  `config_generate_engine_id` and `config_ensure_server_secret` (05 §1.1) after the overrides.
  Loop wiring (05 §1.4 steps 2, 8, 9): `loop_init` over the runtime before the IO provider
  starts, `posix_stop_signals_install`, `loop_listen` on `cfg.host` for `port` and `port + 1`
  (a failure refuses startup), `loop_run`, then `loop_deinit` and
  `posix_stop_signals_restore`.
- [ ] **20. End-to-end protocol tests**: real WebSocket client tests from the 02 message
  catalog, refusal cases, multi-client command → event checks (05 §5).

## Phase 4 — Provisional / blocked areas

- [ ] **21. Composite block types — runtime + `types/` persistence** (03 §5–§5.1). The
  `graph:*group` wire commands are blocked (open question 4).
- [ ] **22. Simulated IO provider + `io/*` blocks + binding table** — **blocked** (open
  questions 3, 6). IO events reach the engine through `pal_handoff_push` (01 §0.2); dispatch
  `WORK_IO_EVENT` in `runtime_drain`.
- [ ] **23. `process/PID`** — **blocked** (open question 5).
- [ ] **24. Schedule blocks** — **blocked** (open questions 5, 11).
- [ ] **25. Zephyr port** (`src/platform/zephyr/` + Zephyr module) — after phase 3.
