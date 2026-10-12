# 02 — WebSocket Wire Protocol

This is the protocol the browser frontend speaks. Most of it is a live contract: the frontend already implements it and should not need to change. A few areas — authentication (which changes the login messages, so the frontend's login code must be updated) and the group/composite-block commands — have no real frontend implementation yet, so they're specified as a clean design rather than constrained by anything existing; those are marked explicitly. Everywhere else, treat this document as the actual wire format, not a target to approximate.

## 0. Transport

Two TCP ports, both set in the configuration file (`05-app-lifecycle-persistence.md` §1.1):
- `port` (default `8080`) — the web server serving the frontend's static assets only (`05-app-lifecycle-persistence.md` §3).
- `port+1` — the FBP WebSocket, at `ws://<host>:<port+1>/` (`wss://` when TLS is enabled, §2.5).

The user enters `http://<host>:<port>/` in the browser; the frontend derives the WebSocket port as the page's own port + 1, so the WebSocket port never has to be entered separately.

One WebSocket per browser tab. Text frames only, one JSON object per message, no batching. The server accepts at most `max_connections` concurrent WebSocket connections (configuration file) and a maximum message size of `max_message_bytes` (a fragmented message counts in total, so this also bounds every frame); an upgrade beyond the connection limit is refused with HTTP 503, and an oversized message closes the connection with WebSocket close code 1009. The upgrade request's `Origin` header must match the host and web port the frontend was served from (or be one of the configured `allowed_origins`); otherwise the upgrade is refused with HTTP 403. This blocks cross-site WebSocket hijacking from other web pages open in the same browser.

Implementation decisions (plan task 14; the server `src/engine/ws_server.h` on the engine loop of `01-architecture-overview.md` §3.2.1, the socket-free pieces `src/protocol/websocket.h`, the upgrade request parsed by `src/protocol/http.h` of `05-app-lifecycle-persistence.md` §3). RFC 6455, server side, the same portable code on both build trees:
- **Startup**: `ws_server_open_config` opens the listener on `host`, `port + 1` with `max_connections`, `max_message_bytes` and `allowed_origins` (startup step 8, `05-app-lifecycle-persistence.md` §1.4; a failure refuses startup); `ws_server_close` after the loop returns.
- **Upgrade**: the request must be `GET` over HTTP/1.1 with an origin-form target (any path; the query is ignored) and no body, else `400`; request heads are limited and parsed as for the web server (8 KiB, 32 fields; `414`/`431`/`400`/`505`). The checks, in order: `Upgrade` must list `websocket`, else `426` with `Upgrade: websocket` and `Sec-WebSocket-Version: 13` — this is also the answer to a plain HTTP request on this port, which serves nothing else; `Connection` must list `Upgrade` (`400`); exactly one `Sec-WebSocket-Version`, `13`, else `426` with `Sec-WebSocket-Version: 13`; exactly one `Sec-WebSocket-Key`, the strict base64 of 16 bytes, else `400`; the `Origin` check (`403`); the connection limit (`503`). Token lists (`Connection: keep-alive, Upgrade`) and their case do not matter. Success is `101` with `Sec-WebSocket-Accept` (base64 of the SHA-1 of the key and the RFC's GUID, `system/crypto.h`). No subprotocol is selected (`Sec-WebSocket-Protocol` is ignored) and no extension is negotiated (so no compression). A refusal is a small `text/plain` response with `Connection: close`, then the connection is closed.
- **Origin**: exactly one `Origin` field, which must be the server's own origin or equal one of `allowed_origins` (ASCII case-insensitively, as a whole; no normalisation); otherwise `403`. The own origin is `<scheme>://<host>[:<port>]` with nothing after it (no path, no userinfo), where the scheme is `http` (case-insensitive; `https` once TLS exists), the host equals the host of the request's `Host` field (its port removed) case-insensitively, an IPv6 address compared as text inside its brackets on both sides, and the port — or the scheme's default (80, 443) when absent — is the web port, this listener's port − 1. The `Host` field's own port (the WebSocket port) is not compared. **A request without `Origin` is refused (`403`)**, as is one with several: browsers always send it on a WebSocket upgrade, and a non-browser client (a test tool, a script) can send the device's own origin, so there is no exception to the rule. `Origin: null` matches only an `allowed_origins` entry `null`.
- **Connection limit**: `max_connections` counts **upgraded** connections, from the `101` until the socket closes (those in the close handshake included); connections still in the handshake do not count. An upgrade beyond the limit is `503`. The listener's hard cap (the loop's, `01-architecture-overview.md` §3.2.1) is `max_connections + WS_PENDING_MAX` (4, a per-build override), so a client beyond the limit still gets its `503`; a TCP connection beyond the hard cap is closed unseen. A handshake must complete within 10 s (`408` after a partial request, a silent close without one).
- **Frames**: client frames must be masked (else close 1002); server frames are never masked. RSV bits (no extension is negotiated), unknown opcodes, a fragmented or longer than 125-byte control frame, a continuation without a message, a new data frame inside a fragmented message, or a 64-bit length with its top bit set: 1002. A binary message: 1003. Fragmented text messages are reassembled; a message larger than `max_message_bytes` in total is 1009, detected from the frame header before its payload is buffered (so a message exactly `max_message_bytes` long passes). Text must be UTF-8 (RFC 3629: no overlong forms, no surrogates, nothing above U+10FFFF; U+0000 allowed): an invalid byte is 1007 as soon as it arrives, a sequence cut by the end of the message too. A ping is answered with a pong carrying its payload, also between the fragments of a message; a pong is ignored.
- **Close**: a client's close frame is answered with a close frame echoing its status code (an empty close without one; a code outside 1000–1003, 1007–1014 and 3000–4999 is answered 1002, a reason that is not UTF-8 1007), then the server closes the TCP connection. A close by the server (the protocol layer's, or a violation above) sends a close frame with the code and, for a violation, a short reason, then waits up to 5 s for the client's close frame — frames meanwhile are read only to find it, their contents dropped — and closes. Before every close, the input already received is read and dropped (at most 64 KiB), so the close sends FIN rather than a reset that could destroy the close frame in flight.
- **Liveness**: after 30 s without any byte from the client, the server sends a ping; if nothing arrives within 15 s more, the connection is aborted (the peer is gone, e.g. a laptop that slept). This is independent of the session idle timeout (§2.5 item 3), which counts client messages, not pongs. The handshake, liveness and close deadlines are checked by one tick hook of the loop (`01-architecture-overview.md` §3.2.1); session expiry (plan task 16) uses its own.
- **Protocol layer API** (plan task 17): `on_open`, `on_message` (one complete, valid UTF-8 text message, NUL-terminated after its length), `on_close` (once for every connection that saw `on_open`, whatever the cause, after which the connection is freed, so its session ends, §2.5 item 2), and `on_drained` (the outgoing buffer has been written out); per connection `ws_send_text`, `ws_send_space`, `ws_send_capacity` (the largest message the empty buffer takes, for the reserve of "Outgoing buffer" below), `ws_send_pending` (bytes not written out yet: 0 means no `on_drained` will follow), `ws_close` (code and reason), the peer address (`ws_peer`, for rate limiting, §2.5 item 7) and a user context pointer. No message is delivered after a close starts.
- **Outgoing buffer**: one fixed buffer per connection (`01-architecture-overview.md` §3.2.1, §4): 512 bytes during the handshake, `WS_OUT_CAP` from the upgrade on — **64 KiB by default, a per-build override**. Every message is framed into it whole; a message that does not fit closes the connection at once, without a close frame (its buffer is full), and `on_close` follows; the client reconnects and starts from a fresh snapshot. Lossy traffic — `network:data` live values (§4.2) and the log mirror's lines — never takes the last quarter of the buffer (the **reserve**, 16 KiB of the default; decided after plan task 20): it is held for replies and events, so a value stream into a client that has stopped reading cannot crowd out the client's next reply or the next event. A burst such as the `graph:connect` snapshot (§4.1.1) must therefore fit the free space (`ws_send_space`); since the snapshot is generated at one instant, the protocol layer may wait for `on_drained` (nothing written meanwhile) and then send it whole, still atomic. A snapshot message is roughly 100–300 bytes (a `graph:addnode` with its metadata, a `graph:addedge`), so 64 KiB holds the snapshot of a graph of roughly 250–500 blocks, links and initials together; a graph whose snapshot exceeds the whole buffer cannot be connected to: `graph:connect` answers `"Graph too large to send: <id>"` and the connection stays open (decided, plan task 18, §4.1.1). **Memory per upgraded connection**: `WS_OUT_CAP`, the reassembly buffer while a message arrives (up to `max_message_bytes` + 1; one above 4 KiB is released after its message) and about 1.1 KiB of connection state; during the handshake instead the 8 KiB request head buffer and 512 bytes. With the defaults (4 connections, 64 KiB, 16 KiB messages) the worst case is about 325 KiB, acceptable on POSIX but not on an MCU: the Zephyr build (plan task 25) sets a smaller `WS_OUT_CAP` — e.g. 16 KiB, about 130 KiB for 4 connections at the default `max_message_bytes`, which suits the RP2354's 520 KiB of SRAM and the STM32H7/N6 — trading the largest graph a client can connect to (about 60–150 snapshot messages at 16 KiB) for RAM.
- **Fairness**: a connection is read at most 16 KiB per loop iteration.
- **TLS** (`tls.enabled`, §2.5 item 9) is not implemented yet: `ws_server_open_config` refuses it with an error, so the engine refuses to start rather than serve plain `ws://` where `wss://` was configured.
- **Logging**: `info` lines for an open and a close (peer address and port, the close code and cause) and for a refused upgrade (peer, status, cause; the refused `Origin` value is the only header field ever logged); a violation is logged at `info` with its cause. Never a message's contents (they carry secrets, §2.5 item 8).

## 1. Envelope

### Client → Server
```json
{ "protocol": "environment|runtime|network|graph|component|trace",
  "command": "<string>",
  "payload": { ... },
  "secret": "<string>" }
```
All four fields are required. `payload` is always a JSON object (`{}` for commands with no arguments). `secret` is always a string — empty (`""`) before login, the issued token afterward (§2.5). A message missing or mistyping any field is rejected with:
```json
{"protocol":"network","command":"error","payload":{"stack":"","message":"Badly formatted request: <raw text>"}}
```

### Server → Client
```json
{"protocol": "...", "command": "...", "payload": { ... }}
```
Server-originated messages never include a `secret` field at all (not even `null`) — the key is simply absent. Two messages deviate from the `{protocol,command,payload:object}` shape and must be special-cased by any strict client:
- `component:componentsready` — payload is a bare JSON integer, not an object: `{"protocol":"component","command":"componentsready","payload":7}`.
- `environment:logout` reply has no `"payload"` key at all: `{"protocol":"environment","command":"logout"}`.

### Error format (every failure, across all sub-protocols)
```json
{"protocol":"network","command":"error","payload":{"stack":"","message":"<text>"[,"graph":"<id>"]}}
```
`"stack"` is always `""`. `"graph"` is present only when a graph id was known at the error site. Every sub-protocol's failures — environment, runtime, graph, component, trace — are reported as `network:error`; there is no `graph:error`/`component:error`/`runtime:error`.

### 1.1 Command → event model: the engine is the single source of truth

**This is the central rule of the protocol.** A client never changes its own view of a graph when it sends a command. The flow is always:

1. The user acts in the UI (e.g. drops a node). The frontend sends the command (`graph:addnode`) and **changes nothing locally**.
2. The engine validates the command and applies it to its model, or refuses it (§3.4.1).
3. If it was applied, the engine broadcasts the resulting event (`graph:addnode`) to **every connection subscribed to that graph** (§4.1). This includes the connection that sent the command; the sender gets no separate acknowledgement.
4. Every frontend, the sender included, updates its view only when it receives the event.

Consequences the implementation must honor:
- **Events describe what was applied, not what was requested.** An event carries the engine's resulting state, which may differ from the request. For example, `graph:addnode` includes defaulted `x`/`y` when the request had no metadata, and `graph:changenode` carries the full merged metadata. A client must render the event's content, never its own request.
- **One order for everyone.** The engine is single-threaded (`01-architecture-overview.md` §3.2), so commands are applied in arrival order and every subscribed connection receives the resulting events in that same order. When two users issue conflicting commands (e.g. both add node `X`), the first one wins and is broadcast; the second is refused and only its sender gets a `network:error`. Nobody's view diverges.
- **A refused command produces no event.** Since the sender never changed its view, there is nothing to roll back. The `network:error` only informs the user.
- **A compound effect is broadcast as its individual events**, in the order applied. For example, removing a node first broadcasts a `graph:removeedge`/`graph:removeinitial` for each attached connection, then the `graph:removenode`, so every client's view passes through only valid states.
- **The sender isn't special-cased.** Events are routed purely by subscription (§4.1), so a client must be connected to a graph (`graph:connect`) to see the results of its own edits to it.
- The same rule applies outside `graph:*`: `runtime:new_graph`/`delete_graph`, `network:start`/`stop`, and group/type changes are all reflected only through their broadcast events. Requests that change nothing — `graph:connect`, `network:edges`, `component:list`, `trace:*` — get a direct reply to the sender instead; `network:getstatus` and `graph:list` are answered with `network:status` broadcasts to every authenticated connection (§3.3, §3.4).

### 1.2 Implementation decisions (plan task 17)

The dispatcher is `src/protocol/dispatcher.h` (one session per WebSocket connection, plugged into the server of §0 as its `WsHandler`), the messages `src/protocol/messages.h`. These fill gaps of §1–§3 without changing any message shape the frontend uses:
- **Order of checks** for each client message: the authorizer's idle activity (§2.5 item 3; every message, a badly formatted one included); the envelope; secret gating (§2.5 item 5); a command not in §3 → the generic error; the capability (§2.4); the command.
- **Envelope**: the text must be one JSON object with nothing but whitespace after it (JSON nested deeper than the parser's limit, `third_party/cjson/README.md`, is not valid). `protocol` must be one of the six names exactly (case-sensitive), `command` a string, `payload` an object, `secret` a string; any other member (e.g. an upstream `requestId`) is ignored. **The raw text in `"Badly formatted request: <raw text>"`** is the message's first 256 bytes (cut at a UTF-8 character boundary, and at a U+0000), followed by `...` when it was cut; it is JSON-escaped like any string. This bounds the reply (a 16 KiB message of control characters would escape to 96 KiB, more than the outgoing buffer). It goes to the sender only and is never logged.
- **Generic error** (an unknown command in any sub-protocol, `environment` included; also the group commands of §3.4.2, `graph:addgroup`/`removegroup`/`renamegroup`/`changegroup`, until they are implemented in plan task 21b): `"Unknown command: <protocol>:<command>"`, the command cut to 64 bytes (character boundary, `...` appended). It comes after secret gating, so before login an unknown gated command answers `"Not authenticated"`.
- **Payload fields**: a required field that is missing, or any field of the wrong type, is refused with `"Invalid field: <name>"` (every command; plan task 18: the fields are checked in the order this document lists them, all before any refusal of §3.4.1, and a structured member — `src`, `tgt`, `metadata`, `edges` — is named as a whole). An optional field that is `null` is the same as absent. Exception: `environment:challenge`/`login` answer `"Login failed"` (§2.3). Payload members a command does not use are ignored.
- **`"graph"` in an error** is present when the command named a valid graph id (1–64 bytes), e.g. `"Graph exists: g1"` and `"Unknown graph: zz"` carry it, `"Invalid id"` does not; never for `"Not authenticated"`, `"Not permitted: …"`, the generic error, or a badly formatted request.
- **Server messages** are compact JSON (no whitespace), one per WebSocket text message, with payload keys in the order this document lists them; numbers carrying engine values are printed exactly (`src/graphs/graph_json.h`).
- **Out of memory** while building a reply closes the connection (WebSocket close code 1011): a reply would otherwise be missing silently. The client reconnects. An authorizer refusing a new connection (`open` fails) closes it the same way.
- **Bursts** (§0 "Outgoing buffer"): the `getruntime` (after login) and `component:list` replies are sent whole only if all their messages fit the outgoing buffer's free space, counting 10 bytes of framing per message; otherwise the reply is made again when the buffer has drained (`on_drained`), then with the state of that moment. A reply that does not fit even an empty buffer is answered `"Reply too large"` instead. A deferred reply may be overtaken by other replies and broadcasts meanwhile. Broadcasts are never deferred: one that does not fit closes the connection (§0). The `graph:connect` snapshot is deferred the same way, with its own refusal and scoping rules (§4.1.1); `network:data` coalesces instead (§4.2). **A deferred burst takes precedence over lossy traffic**: while one waits for `on_drained`, no `network:data` is sent (the values coalesce) and no log line is mirrored, so a value stream cannot keep the buffer from ever draining and starve the burst; at the drain the burst goes first, the coalesced values after it (§4.2 "Back-pressure").

## 2. Authentication and authorization

> **This area is a clean design.** The existing frontend has only a basic login form that sends `environment:login {user,password}`. The design below replaces that with a challenge-response login, so the password never crosses the network even without TLS. **The frontend's login code must be updated to match** (allowed per `00-README.md`, "Protocol compatibility policy"). Every other message still carries the top-level `secret` string exactly as before.

### 2.1 Goals and threat model

The device runs on a local network, usually without TLS (§2.5). The design therefore assumes a passive eavesdropper on the LAN and hostile web pages open in the user's browser, and aims for:
- the password is never sent, in any form an eavesdropper can replay or brute-force cheaply;
- the device never stores anything password-equivalent;
- the expensive key-stretching runs in the browser, not on the MCU (on the device a login costs a few SHA-256 operations);
- a stolen session secret is useless on any other connection.

The scheme is SCRAM-SHA-256 (RFC 5802/7677), reduced to two round trips over the existing envelope.

### 2.2 Credential store

One record per user:

| Field | Content |
|---|---|
| `user` | user name (UTF-8, 1–32 bytes) |
| `role` | `admin`, `editor`, `operator`, or `viewer` (§2.4) |
| `salt` | 16 random bytes, base64 |
| `iterations` | PBKDF2 iteration count chosen when the password was set (default `100000`) |
| `stored_key` | `SHA256(ClientKey)`, base64 |
| `server_key` | `HMAC(SaltedPassword, "Server Key")`, base64 |

where `SaltedPassword = PBKDF2-HMAC-SHA256(password, salt, iterations, 32 bytes)` and `ClientKey = HMAC(SaltedPassword, "Client Key")`. Neither the password nor `SaltedPassword` is stored. Knowing `stored_key` does not let someone log in.

- **Location**: the users file (`users.json`, JSON array of the records above). It lives in the configuration directory on POSIX (`<confdir>/users.json`) and at `/lfs/config/users.json` on Zephyr (`05-app-lifecycle-persistence.md` §1). It is written atomically like every other persisted file (`05-app-lifecycle-persistence.md` §2.3).
- **Provisioning**: users are created and changed only locally, never over the network in this version:
  - POSIX: `hakkas user add <name> --role <role>`, `hakkas user passwd <name>`, `hakkas user del <name>`, `hakkas user list` (the password is read interactively from the terminal, never from argv).
  - Zephyr: the same commands in the Zephyr shell over the console/USB-CDC backend. Physical access to the console is the trust anchor.
- **No default credentials**: nothing is compiled into the binary. If the users file is missing or empty, every login is refused and the web server's root page still loads, but the frontend can only show "no users provisioned".

Implementation decisions (plan task 16, `src/protocol/users.h`):
- **Format**: a JSON array of objects with exactly the six members above — each required, no other member allowed (the file is written only by hakkas), a member repeated is an error. `salt` is the strict base64 (RFC 4648 §4, padded, no whitespace) of exactly 16 bytes, `stored_key`/`server_key` of exactly 32; `iterations` an integer 1–4294967295 (any JSON number form with an integral value). Written by the user commands as cJSON's indented form, members in the table's order, with a final newline.
- **User names** are 1–32 bytes of valid UTF-8 **without control characters** (U+0000–U+001F, U+007F), compared byte for byte (case-sensitive). The control-character rule is a tightening of "UTF-8, 1–32 bytes": names are logged (§2.5 item 8), so one must never break a log line.
- **Missing, empty, malformed**: a missing file, an empty one (no bytes, or only whitespace) and `[]` all mean no users. Anything else wrong — not JSON, not an array, a record breaking a rule above, **two records with the same name**, more than `HAKKAS_USERS_MAX` records, more than `512 × HAKKAS_USERS_MAX + 64` bytes — makes the **whole file** malformed: no record of it is used (a partly used file would silently drop users). The error is logged (which record and member, never a value). **The engine still starts, with no users** (every login refused; a warning for a missing or empty file, an error for a malformed or unreadable one): refusing to start would protect nothing more — both refuse every login — but would stop the graphs controlling the plant until someone repairs a file that only gates logins. The user commands never overwrite a malformed file: they fail and leave it as it is.
- **Bounded**: at most `HAKKAS_USERS_MAX` users (default 32, a per-build override), held in a fixed table (about 120 bytes each).
- **When it is read** (decided, plan task 18; plan task 16 read it at startup only): at startup (`05-app-lifecycle-persistence.md` §1.4 step 3), and again **whenever a challenge is made** (`environment:challenge`; a challenge delayed by the back-off, §2.5 item 7, when it is made). A user added, removed or given a new password with the user commands while the engine runs therefore takes effect at the next login, without a restart; sessions already open are not affected (they last until they expire or log out). A login is checked against the users of the latest read (a password changed between a challenge and its login fails that login). At such a re-read a missing, empty or malformed file means **no users**, exactly as at startup — the file is written only atomically by the user commands, so a malformed one comes from a hand edit, and failing closed never keeps a user the edit meant to remove; the parse error is logged as at startup, plus a warning that every login is refused. A file that **cannot be read** (a storage error, or no memory for the read) keeps the users of the previous read, with a warning: a transient read error does not lock everyone out. The authorizer takes the reader as a parameter (`AuthConfig.load_users`, `users_loader` with the file's path; a fake in tests), so it stays testable. Reading is cheap: a file of at most `512 × HAKKAS_USERS_MAX + 64` bytes, parsed into a temporary store on the heap.
- **Deriving**: `user add`/`passwd` draw a fresh 16-byte salt from the PAL's random source, use the default iteration count (`100000`), store only `stored_key` and `server_key`, and zeroize the password and every intermediate value.

### 2.3 Login handshake

The connection keeps an auth state: `UNAUTHENTICATED → CHALLENGED → AUTHENTICATED`.

**Step 1 — `environment:challenge`** (client → server, `secret: ""`):
```json
{"protocol":"environment","command":"challenge","payload":{"user":"alice","cnonce":"<base64, 16+ random bytes>"},"secret":""}
```
Reply (server → client):
```json
{"protocol":"environment","command":"challenge","payload":{"salt":"<base64>","iterations":100000,"nonce":"<cnonce + base64 of 16 server-random bytes>"}}
```
- The full `nonce` is the client nonce followed by the server's random part. It is valid for one login attempt on this connection only, and expires after 30 s.
- **Unknown users get a normal-looking reply**: the server returns `salt = HMAC(server_secret, user)[0..16]` and the default iteration count, where `server_secret` is a random value generated once and stored with the configuration (`auth.server_secret`, `05-app-lifecycle-persistence.md` §1.1: 32 random bytes in base64; the HMAC key is the stored string's bytes, not decoded). The reply for an unknown user is indistinguishable from a real one, so user names can't be probed.

**Step 2 — `environment:login`** (client → server, `secret: ""`):
```json
{"protocol":"environment","command":"login","payload":{"user":"alice","nonce":"<full nonce>","proof":"<base64>"},"secret":""}
```
The client computes:
```
SaltedPassword  = PBKDF2-HMAC-SHA256(password, salt, iterations)
ClientKey       = HMAC(SaltedPassword, "Client Key")
StoredKey       = SHA256(ClientKey)
AuthMessage     = user + "," + nonce + "," + salt + "," + iterations
ClientSignature = HMAC(StoredKey, AuthMessage)
proof           = ClientKey XOR ClientSignature
```
The server recomputes `ClientSignature` from its `stored_key`, recovers `ClientKey = proof XOR ClientSignature`, and accepts only if `SHA256(ClientKey) == stored_key`, using a constant-time comparison.

Success reply:
```json
{"protocol":"environment","command":"login","payload":{"secret":"<token>","role":"editor","capabilities":["..."],"server_signature":"<base64 HMAC(server_key, AuthMessage)>"}}
```
The client should check `server_signature`, which proves the device knows the user's `server_key` and isn't an impostor.

Any failure (unknown user, wrong proof, expired or reused nonce, login without a preceding challenge) → `network:error` with the message `"Login failed"`. The message never says which part failed.

Implementation decisions (plan task 16, `src/protocol/auth.h`):
- **Challenge**: `user` must be a valid user name (§2.2) and `cnonce` the strict base64 of 16–64 bytes; otherwise the reply is `"Login failed"` (not counted as a failed login, item 7). The server's part of the nonce is the base64 of 16 random bytes (24 characters). A new challenge replaces the connection's previous one (its nonce is dead). A challenge on an `AUTHENTICATED` connection is refused with `"Login failed"` (not counted): the client logs out first. For an unknown user the salt is the first 16 bytes of `HMAC-SHA256(server_secret, user)` (the user name's UTF-8 bytes), base64, with `100000` iterations, so a reply for an unknown user differs from a real one only if the real user's iteration count is not the default.
- **Login**: `user` and `nonce` must equal the challenge's (`nonce` compared in constant time) and `proof` must be the base64 of exactly 32 bytes. `salt` in the AuthMessage is the base64 text sent in the challenge and `iterations` its decimal form, so `AuthMessage = "alice,<cnonce><server part>,<salt base64>,100000"`. **Any login attempt uses up the challenge**, right or wrong. For an unknown user the server runs the same HMAC/SHA-256 check against a key derived from `server_secret`, so the failure takes the same time as a wrong password. A failure leaves the connection `UNAUTHENTICATED` (or `AUTHENTICATED` if it was, for a stray login without a challenge).
- The `secret` is issued only after the proof has been accepted; if the random source fails then, the reply is `"Login failed"` and the connection is `UNAUTHENTICATED`.

**Browser note**: `crypto.subtle` (WebCrypto) is only available in secure contexts (HTTPS or `localhost`). On a plain `http://<device-ip>` page the frontend must bundle a small JavaScript SHA-256/HMAC/PBKDF2 implementation. With a pure-JS PBKDF2, the frontend should expect `iterations` in the 10⁴–10⁵ range to take on the order of a second.

### 2.4 Roles and capabilities

The role in the user's record determines which FBP capabilities (upstream `fbp-protocol.html`, "Capabilities") the session gets. The server checks the capability required by each command before running it:

| Role | Capabilities | Can do |
|---|---|---|
| `viewer` | `protocol:component`, `network:status`, `network:data`, `graph:readonly` | see graphs, components, status, live values |
| `operator` | viewer + `network:control` | also start/stop graphs |
| `editor` | operator + `protocol:graph`, `protocol:runtime`, `network:persist` | also edit/create/delete/persist graphs |
| `admin` | editor (reserved for future remote user management) | same as editor for now |

Command → required capability: `graph:connect`/`graph:list`, `component:*`, `trace:*` → `graph:readonly` or `protocol:graph`; `network:getstatus` → `network:status`; `network:edges` → `network:data`; `network:start`/`stop` → `network:control`; `network:persist` → `network:persist`; graph-mutating `graph:*` → `protocol:graph`; `runtime:new_graph`/`delete_graph` → `protocol:runtime`; `runtime:getruntime` → none. A command whose capability the session lacks → `network:error` `"Not permitted: <protocol>:<command>"`.

Implementation decisions (plan task 16, `auth_command_rule`/`auth_permits` in `src/protocol/auth.h`, which the dispatcher of plan task 17 calls):
- The **read-only graph commands** are exactly `graph:connect` and `graph:list`. **Graph-mutating** are every other graph command of §3.4 and §3.4.2: `rename`, `addnode`, `removenode`, `renamenode`, `changenode`, `addedge`, `removeedge`, `changeedge`, `addinitial`, `removeinitial`, `addinport`, `removeinport`, `renameinport`, `addoutport`, `removeoutport`, `renameoutport`, `addgroup`, `removegroup`, `renamegroup`, `changegroup`.
- `component:*` is `list` and `getsource`; `trace:*` is `start`, `stop`, `clear`, `dump` (§3.5, §3.6).
- A command not in §3 (e.g. `graph:clear`) needs no capability: it is secret-gated like any other (except within `environment`, which is never gated), then answered with the generic error.
- `allCapabilities` order (§3.2.1) is also the order in which a session's capabilities are listed (`login` reply, `runtime:runtime`).

### 2.5 Sessions and secrets

1. **Secret format**: 128 bits from the PAL's cryptographic random source, base64url-encoded (22 chars).
2. **Binding**: a secret is valid only on the connection it was issued to. A secret presented on any other connection is invalid. Closing the connection destroys the session.
3. **Lifetime**: sessions expire after `auth.idle_timeout_s` without a client message (default 1800 s) and after `auth.max_lifetime_s` in total (default 43200 s). On expiry the server sends `{"protocol":"environment","command":"logout"}`, stops event delivery, and the connection returns to `UNAUTHENTICATED` (the socket stays open so the client can log in again).
4. **Logout**: `environment:logout` invalidates the connection's session and returns it to `UNAUTHENTICATED`.
5. **Secret gating**: every message except `environment:challenge`, `environment:login`, and `runtime:getruntime` must carry the connection's current secret. An invalid, expired, or missing secret → `network:error` `"Not authenticated"`. The submitted value is never echoed back.
6. **Event delivery requires authentication**: graph-change broadcasts, `network:data`, and log mirroring are only delivered to `AUTHENTICATED` connections. There is no passive, pre-login event stream.
7. **Rate limiting**: a connection is closed after 3 failed logins. Per source address, consecutive failures trigger exponential back-off: after the 5th failure, further challenges from that address are delayed 1 s, doubling up to 60 s, and the counter resets after a success or 15 min of quiet. The table is bounded (default 16 addresses, least-recently-used eviction) so memory use is fixed on the MCU.
8. **Logging**: never log secrets, proofs, nonces, salts, or passwords. Log the fact of a login success or failure with the user name and source address only.
9. **TLS (optional, default off)**: Zephyr ships TLS through its mbedTLS integration (TLS socket options in its socket API), and on POSIX OpenSSL/mbedTLS is available, so no custom TLS is needed. When `tls.enabled` is set, both ports serve TLS (`https://` / `wss://`) using a certificate and key from the configuration directory. It is off by default because of the RAM cost on the MCU and the browser warnings a self-signed LAN certificate causes. The handshake above is designed to stay safe without it. Not implemented yet (plan task 14, §0): with `tls.enabled` set the engine refuses to start. Without TLS, an eavesdropper can still see the traffic after login (graph contents, values), but can't recover the password or reuse the session.
10. **Testability**: the authorization component is a small module behind an interface (`challenge`, `verify_proof`, `issue_secret`, `validate_secret`, `revoke_secret`, `capabilities_for`), so protocol-handler tests can substitute a fake implementation. Integration tests that need the real flow use a deliberately provisioned test users file, never a credential compiled into the build.

Implementation decisions (plan task 16, `src/protocol/auth.h`):
- **Interface** (item 10): `Authorizer` is a table of operations (`AuthOps`) and its object. Besides the six above it has `open` (a connection opened, with its peer address and the protocol layer's callbacks), `close` (the connection closed: its session ends, item 2), `activity` (a client message arrived, item 3) and `state`. `verify_proof` accepts the proof (the connection is then `VERIFIED`, an intermediate state) and `issue_secret` turns that into the session. `capabilities_for` is non-zero exactly when the connection is `AUTHENTICATED` with a live session, which is also the test for event delivery (item 6). The real implementation takes the clock and the random source as parameters (no PAL call), so tests run it with controlled time.
- **Expiry** (item 3): the idle timeout counts every client message (`activity`, called by the protocol layer for each message — not the pongs to the WebSocket server's pings, §0). A session is dead from the instant its idle or lifetime deadline passes: from then `validate_secret` is false and `capabilities_for` 0. Its own tick hook (`auth_tick`, `01-architecture-overview.md` §3.2.1) returns the connection to `UNAUTHENTICATED` and calls the protocol layer's `on_expired`, which sends `environment:logout` and stops event delivery; the expiry log line names the cause (idle timeout or maximum lifetime). The same hook drops expired challenges. A logout or a close ends a session without `on_expired`.
- **What counts as a failed login** (item 7): every `environment:login` that does not succeed — no challenge, an expired or used nonce, a user or nonce differing from the challenge's, a malformed or wrong proof, an unknown user. A refused `environment:challenge` does not count.
- **Closing after 3 failures**: the third failed login on a connection is answered `"Login failed"`, then the connection is closed with WebSocket close code 1008 (policy violation). Failures on a connection are not reset by a success.
- **Back-off**: per source address (`ws_peer(conn)->addr`), with `f` consecutive failures and `f ≥ 5`, a challenge is answered no earlier than `min(60 s, 1 s × 2^(f−5))` after the address's **last failure** (so 1, 2, 4, 8, 16, 32, 60, 60 … s after the 5th, 6th, 7th … failure). Counting from the last failure rather than from the request spares a person who took longer than the back-off to retype a password; an automated client gains nothing. **The challenge is deferred, not refused**: the authorizer answers `AUTH_DELAYED`, keeps the request (one per connection) and its tick hook makes the challenge when it is due and hands it to the protocol layer (`on_challenge`), which sends it as the normal `environment:challenge` reply — on the wire the reply just arrives late, and the loop never blocks. A new challenge meanwhile replaces the pending one (same due time); a login meanwhile has no challenge and fails. A success from the address, or 15 min without a failure from it, resets its count. The table holds `HAKKAS_AUTH_RATE_ADDRS` addresses (default 16, a per-build override); an address enters it on its first failure, evicting the least recently used entry when full.
- **Dispatcher** (plan task 17): the reply to `AUTH_FAILED_CLOSE` is `"Login failed"`, then the close with code 1008 and the reason `too many failed logins`. A login that succeeds starts event delivery (§4.1); a logout or an expiry stops it, and an expiry sends the logout message of item 3 from the authorizer's tick. Event delivery and `runtime:runtime`'s capabilities both use `capabilities_for`, so nothing is delivered between a session's deadline and its tick. The issued secret appears only in the `login` reply.
- **Logging** (item 8): `info` for a login (`login: user '<name>' from <address> as <role>`), a logout, a session's end (expiry with its cause, or the connection's close), a back-off and a connection closed after 3 failures; `warn` for a failed login (`login failed: user '<name>' from <address>`). A user name that is not valid (§2.2: e.g. with a control character) is logged as `(invalid name)`. Never a secret, proof, nonce, salt, password, key or `server_secret`.

## 3. Protocol namespaces

Every command below requires an authenticated connection and the capability listed in §2.4, unless noted otherwise.

### 3.1 `environment` — `challenge`, `login`, `logout` (§2). Not secret-gated. Anything else → generic error.

### 3.2 `runtime`

| command | request payload | behavior |
|---|---|---|
| `getruntime` | `{}` | **Allowed before login.** Before login: sends only the `runtime:runtime` push (§3.2.1) with `"capabilities":[]`, so a client can discover the runtime and see that it must log in. After login: sends `runtime:runtime` with the session's capabilities; one `component:component` per registered block type; one `component:componentsready` with the count; and triggers a `network:status` broadcast for every existing graph. |
| `new_graph` | `{"id"?,"name"?,"description"?,"icon"?}` all optional (`id`→fresh random UUID v4, lowercase, if absent, `name`→`"<<new graph>>"`, others→`""`; decided, plan task 17: `null` is the same as absent, any other non-string → `"Invalid field: <name>"`, §1.2) | Creates the graph (stopped, empty); acknowledgement arrives via the `runtime:new_graph` broadcast (§3.2.2) to every subscriber. Not persisted until `network:persist`. |
| `delete_graph` | `{"id","name"}` both required (decided, plan task 17: a missing or non-string one → `"Invalid field: <name>"`, §1.2) | Stops and deletes the graph **and removes its persisted file** (`05-app-lifecycle-persistence.md` §2.3); acknowledgement arrives via the `runtime:delete_graph` broadcast (§3.2.3). `name` must match the graph's current name (a guard against deleting the wrong graph). A started graph is stopped first, and its `network:stopped` is broadcast before the `runtime:delete_graph` (§1.1, compound effects). |
| other | — | generic error |

**`runtime:runtime` push** (§3.2.1):
```json
{"protocol":"runtime","command":"runtime","payload":{
  "id":"<engine_id from the configuration file; a random UUID per boot if not configured>",
  "label":"<label from the configuration file, default \"Hakkas\">",
  "version":"<build version string>",
  "allCapabilities":["protocol:runtime","protocol:graph","protocol:component","graph:readonly","network:status","network:persist","network:data","network:control"],
  "capabilities":["<the session's capabilities per role, §2.4; [] before login>"],
  "type":"hakkas","namespace":"hakkas","repository":"","repositoryVersion":""
}}
```

**`runtime:new_graph` broadcast** (§3.2.2): `{"protocol":"runtime","command":"new_graph","payload":{"graph":"<id>","name":"<name>","description":"<description>","icon":"<icon>"}}`.

**`runtime:delete_graph` broadcast** (§3.2.3): `{"protocol":"runtime","command":"delete_graph","payload":{"graph":"<id>","name":"<name>"}}`.

### 3.3 `network`

| command | request payload | behavior |
|---|---|---|
| `start` | `{"graph":String}` | Starts the graph (`01-architecture-overview.md` §2.2). Acknowledgement arrives via the `network:started` broadcast (filtered to subscribers of that graph, §4). Starting an already-started graph re-sends `network:started` without restarting; its `time` stays the start of the running run. |
| `stop` | `{"graph":String}` | Stops the graph: events stop propagating, timers are cancelled, structure and input values are kept → `network:stopped` broadcast. Stopping an already-stopped graph re-sends `network:stopped`, with the `time` and `uptime` of the most recent run (epoch and `0` if it never ran). |
| `getstatus` | `{"graph":String}` | → `network:status` broadcast to every authenticated connection (not only the requester). |
| `persist` | `{"graph":String}` | Serializes the graph, including its current started/stopped state, and writes it atomically to storage (`05-app-lifecycle-persistence.md` §2). No wire reply on success; failure broadcasts `network:error` with message `"Unable to save graph: <name> (<id>)"`. |
| `edges` | `{"graph":String,"edges":[{"src":{"node":String,"port":String},"tgt":{"node":String,"port":String}}, ...]}` | Registers live link-value subscriptions — §4.2. Replies to the requester only by echoing the payload back as `network:edges`. |
| other | — | generic error |

Request handling (decided, plan task 18): `graph` is required and must be a string (`"Invalid field: graph"`, §1.2); an invalid or unknown graph id is refused with `"Invalid id"` / `"Unknown graph: <id>"` to the requester only (§3.4.1), for `start`, `stop`, `getstatus` and `persist` alike. `network:edges`: §4.2.

`persist` (decided, plan task 10): an invalid or unknown graph id is a refusal like any other (`"Invalid id"`, `"Unknown graph: <id>"`), sent to the requester only (§3.4.1); only a failed save is broadcast as the `network:error` above, with `"graph"` set. The file is written in the canonical form of `05-app-lifecycle-persistence.md` §2.1.

**Timestamps**: until a wall clock exists (`01-architecture-overview.md` §3.4), every `time` field is the engine uptime at the event, rendered as an ISO8601 UTC timestamp counted from the epoch — e.g. a graph started 90 s after boot reports `"1970-01-01T00:01:30Z"`. Clients must not interpret it as a calendar date. Once a wall clock is available, real UTC timestamps are sent in the same format.

**`network:started`**: `{"protocol":"network","command":"started","payload":{"graph":"<id>","time":"<ISO8601>","started":true,"running":true,"debug":false}}`.
**`network:stopped`**: `{"protocol":"network","command":"stopped","payload":{"graph":"<id>","time":"<ISO8601, the start time of the run that just ended>","uptime":<int seconds that run lasted>,"started":false,"running":false,"debug":false}}`. For a graph that has never been started, `time` is the epoch and `uptime` is `0`.
**`network:status`**: `{"protocol":"network","command":"status","payload":{"graph":"<id>","name":"<name>","description":"<description>","uptime":<int seconds since last start, 0 when stopped>,"running":<bool>,"started":<bool>,"debug":false}}`. `running` always equals `started`.
**`network:data`** (link-value stream, §4.2): `{"protocol":"network","command":"data","payload":{"id":"unknown","graph":"<id>","src":{"node","port"},"tgt":{"node","port"},"data":"<value, stringified>"}}`. Values are always transmitted as strings on this channel, regardless of their underlying type.
**`network:edges`** ack: echoes the request payload back, requester only.
**`network:output`** / **`network:error`** (log/output mirroring, off by default — `05-app-lifecycle-persistence.md` §4): `{"protocol":"network","command":"output","payload":{"type":"message","message":"<text>"}}` sent to every authenticated connection (§2.5), mirroring server log/output text. `network:error` carries the same shape as the standard error format (§1) and is used both for mirrored `error`-level log output and for explicit protocol errors.

### 3.4 `graph`

| command | request payload | behavior |
|---|---|---|
| `list` | `{}` | Triggers a `network:status` broadcast for every graph in the system. |
| `connect` | `{"id":String}` | Scopes this connection's event subscription to this graph id and sends the full graph snapshot to the requester — §4.1.1. |
| `rename` | `{"id","from","to"}` | Changes the graph's **name** (the `id` never changes, so the persisted file keeps its name) and broadcasts a `graph:rename` event `{"id","from","to"}` to subscribers. The event's `from` is the name the graph actually had; the request's `from` is not checked. Any string is a valid name. |
| `addnode` | `{"id","graph","component","metadata"?:{"x","y",...}}` (`id`/`graph`/`component` required) | Creates the block (started immediately if the graph is started). `graph:addnode` broadcast `{"graph","component","id","metadata":{"x","y",...}}`. Missing `x`/`y` default to `50`. |
| `removenode` | `{"id","graph"}` | Removes the block; every input it fed reverts to its default (`01-architecture-overview.md` §2.2). Broadcasts `graph:removeedge`/`graph:removeinitial` for each attached connection first, then `graph:removenode` `{"id","graph"}`. |
| `renamenode` | `{"from","to","graph"}` | `graph:renamenode` broadcast `{"graph","from","to"}`. Links are unaffected. |
| `changenode` | `{"id","metadata":{"x"?,"y"?,...},"graph"}` | Merges the given metadata keys into the node's metadata. `graph:changenode` broadcast `{"id","graph","metadata":{<full resulting metadata>}}`. |
| `addedge` | `{"src":{"node","port"},"tgt":{"node","port"},"graph","metadata"?}` | Connects the output to the input; the input immediately receives the output's current value. `graph:addedge` broadcast `{"graph","src":{"node","port"},"tgt":{"node","port"}[,"metadata"]}`. |
| `removeedge` | `{"src","tgt","graph"}` | Disconnects; the input reverts to its default and queued values on the link are dropped. `graph:removeedge` broadcast, same src/tgt shape. |
| `changeedge` | `{"src","tgt","metadata":{"route"?,"secure"?,"schema"?, ...},"graph"}` | Replaces the edge's metadata. `graph:changeedge` broadcast to every subscriber of the graph (not just the requester) with the updated metadata. |
| `addinitial` | `{"tgt":{"node","port"},"src":{"data":<value>},"graph"}` | Wires a literal to the input (replacing an existing literal on that input); the value is delivered immediately and propagates if the graph is started. `graph:addinitial` broadcast `{"graph","src":{"data":<value>},"tgt":{"node","port"}}`. |
| `removeinitial` | `{"tgt":{"node","port"},"graph"}` | Removes the literal; the input reverts to its port's default value, which propagates. `graph:removeinitial` broadcast `{"src":{"data":<removed value>},"tgt":{"node","port"},"graph"}` to every subscriber. |
| `addinport` | `{"graph","node","port","public":String}` | `graph:addinport` broadcast `{"graph","name":<value of "public">,"node","port"}` — note the request field is `"public"`, the broadcast field is `"name"`. |
| `removeinport` | `{"graph","public"}` | `graph:removeinport` broadcast `{"graph","name"}`. |
| `renameinport` | `{"graph","from","to"}` | `graph:renameinport` broadcast `{"graph","from","to"}`. |
| `addoutport` | `{"graph","node","port","public"}` | `graph:addoutport` broadcast `{"graph","name","node","port"}`. |
| `removeoutport` | `{"graph","public"}` | `graph:removeoutport` broadcast `{"graph","name"}`. |
| `renameoutport` | `{"graph","from","to"}` | `graph:renameoutport` broadcast `{"graph","from","to"}`. |
| other | — | generic error |

Edge metadata is purely for the user interface (routing, colors). The engine stores it opaquely, persists it, and replays it in the `graph:connect` snapshot, but never interprets it.

Metadata decisions (plan task 10; the model side, `src/graphs/graph_json.h`, without wire code until plan task 18):
- **Node metadata** (UI-only, `01-architecture-overview.md` §2.2): `x`/`y` must be finite numbers; every other key — including a `change_threshold` key, which no longer has any effect (open question 7) — is opaque and kept as-is, in its order, after `x`, `y` in events, snapshots and files.
- **`changenode` merge**: each key of the request's `metadata` is merged into the node's metadata, keys not named are kept. `null` removes a key (like JSON Merge Patch, RFC 7396): `"x": null`/`"y": null` reset the position to `50`, and an opaque key set to `null` is deleted. Any other value replaces the key in place (an object value as a whole, not merged recursively) or adds it at the end. A request with an invalid `x` or `y` is refused as a whole.
- **Edge metadata** (`addedge`, `changeedge`): an object, or absent/`null` for none; `changeedge` replaces it entirely (an empty object is kept as `{}`, `null` removes it) and its event carries the resulting metadata — `{}` when the edge has none (decided, plan task 18: `graph:changeedge` always has a `metadata` object, so a client that merges it never meets an absent one; `graph:addedge` and the snapshot leave it out when there is none). The `graph:changeedge` broadcast is graph-specific (§4.1); a `changeedge` for an edge that does not exist is `"No such connection"` (§3.4.1), checked like `removeedge`. Removing an edge drops its metadata.
- **Placeholder blocks** (`05-app-lifecycle-persistence.md` §2.2) appear like any block: `graph:addnode` (events and snapshot) carries the placeholder's original `component`, and the block can be moved, renamed, changed and removed. It has no ports, so an edge, initial or graph port naming one of its ports is refused as `"Unknown port: <node>.<port>"`. The connections and graph ports of the file that reference it are not sent on the wire (they are not live); they are kept for the file only (`01-architecture-overview.md` §2.2).

Graph-level inports/outports define the external ports a graph has when it is used as a group/composite block type (`01-architecture-overview.md` §2.2, `03-block-types.md` §5). They have no dataflow effect in a top-level graph.

Graph ports (decided, plan task 7): inports and outports are separate namespaces (an inport and an outport may share a public name). Several ports may map to the same inner port. A port follows its node: `renamenode` changes the node every port mapped to it refers to, and needs no port event (the `graph:renamenode` implies it). `removenode` removes every port mapped to the node, broadcasting a `graph:removeinport`/`graph:removeoutport` for each (inports, then outports, each in creation order) after the removed edges and initials and before the `graph:removenode`.

Request handling (decided, plan task 18; `src/protocol/dispatcher.c`):
- **Required fields**: every request field the table lists without `?` must be present with its type — a string (`id`, `graph`, `component`, `from`, `to`, `node`, `port`, `public`), an endpoint `{"node":String,"port":String}` (`src`, `tgt`), for `addinitial` a `src` object whose `data` is a string, number or boolean, for `changenode` a `metadata` object — else `"Invalid field: <name>"` (§1.2), checked in the order the table lists them, before any refusal of §3.4.1. The optional `metadata` of `addnode` and `addedge` may be `null`, the same as absent. `changeedge`'s `metadata` must be present, an object or `null` (removes the metadata, above). `rename` needs `id` and `to`; its `from` is ignored entirely (not required, not checked).
- **Invalid metadata** — node metadata whose `x`/`y` is not a finite number (also in a `changenode` patch, checked before the graph and node are looked up), edge metadata that is neither an object nor `null` — is `"Invalid field: metadata"`.
- **Initials**: `data` is parsed for the target input's declared type (below). When the target is no input — an unknown graph, node or port, an output, a placeholder's port — the command is refused as §3.4.1 says, whatever the data.
- **Broadcasts** carry the payload keys in the order of the table above and describe the applied state (§1.1); the request's own values never appear unless they are that state (e.g. `rename`'s `from` is the previous name).
- **Groups**: the commands of §3.4.2 are not implemented yet (plan task 21b) and answer the generic error (§1.2).

**Initial value encoding**: the `data` of an initial is accepted as a JSON string, number, or boolean. A string is parsed according to the target port's declared type (`"number"`: as a number; `"bool"`: `to_bool`; `"string"`: as-is). Details (`01-architecture-overview.md` §2.1): a `"number"` string that is wholly a number becomes an f64 (never an i64 — `"number"` ports default to `0.0`, and JSON numbers are doubles too); any other text (including a former unit form such as `"4.2 V"`) gives `0.0`, the same as `to_f64` of that string. A port type other than `"bool"`/`"string"` parses like `"number"`, matching `default_value`. The `graph:addinitial` event carries `data` as a string, matching what the existing frontend sends. The persisted file stores it as a native JSON value (`05-app-lifecycle-persistence.md` §2).

#### 3.4.1 Invalid commands

There is no error behavior *in the dataflow* (`01-architecture-overview.md` §2.1). A **structural command** that would break the graph's invariants is different: it is refused as a whole. The graph is left exactly as it was, nothing is broadcast, and the requester alone receives `network:error` (with `"graph"` set). Since the frontend only updates its view from events (§1.1), there is nothing to roll back. Refused cases:

| Case | Message |
|---|---|
| unknown graph id | `"Unknown graph: <id>"` |
| unknown node, or unknown port on a node | `"Unknown node: <node>"` / `"Unknown port: <node>.<port>"` |
| `addnode`/`renamenode` to a node id that already exists in the graph | `"Node exists: <id>"` |
| `addnode` with an unregistered component | `"Unknown component: <name>"` |
| `addedge` whose `src` is not an output or whose `tgt` is not an input | `"Invalid edge direction"` |
| `addedge` to an input that already has an edge or an initial (no fan-in) | `"Input already connected: <node>.<port>"` |
| `addinitial` to an input that has an edge | `"Input already connected: <node>.<port>"` |
| `removeedge`/`changeedge`/`removeinitial` for a connection that doesn't exist | `"No such connection"` |
| `addinport`/`addoutport`/`rename*port` to a public name that already exists | `"Port exists: <name>"` |
| `delete_graph` whose `name` doesn't match | `"Graph name mismatch: <id>"` |
| `new_graph` with an `id` that already exists | `"Graph exists: <id>"` |
| graph or node id that is empty or longer than 64 bytes | `"Invalid id"` |
| `removeinport`/`removeoutport`/`rename*port` naming a public name that doesn't exist | `"Unknown port: <name>"` |

Order and edge cases (decided, plan task 6): every node id in the command is checked first (`Invalid id`), then each endpoint in turn, source before target, node before port; the first failure is reported. An `addinitial` whose `tgt` names an output is `"Invalid edge direction"` (the literal's target is not an input). A `removeedge`/`removeinitial` naming a port that exists but has the wrong direction is `"No such connection"` (no such connection can exist); a name that is no port of the node is `"Unknown port"`. `renamenode` to the node's own name is `"Node exists"`.

Further decisions (plan task 7): the graph id is checked first, together with the node ids (`Invalid id`), and the graph is looked up (`Unknown graph`) before any node. A graph port's public name is an id too (1–64 bytes, else `"Invalid id"`). `addinport` naming a port that is not an input of the node — an output included — is `"Unknown port: <node>.<port>"`, likewise `addoutport` with a non-output. `addnode` checks `"Node exists"` before `"Unknown component"`. `rename*port` to the port's own name is `"Port exists"`.

#### 3.4.2 Groups — user-defined block types

> **Decided** (`00-README.md`, open question 4): creating a group is **"save as component"** — the selected nodes are left unchanged. No frontend implementation exists yet, so the wire shapes below are a clean design (`00-README.md`, "Protocol compatibility policy"). A "collapse" option that also replaces the selection by an instance may be added later. The commands below reuse the standard FBP group commands with the meaning this system gives a group: a **reusable user-defined block type** built from a selection of blocks (`03-block-types.md` §5).

| command | request payload | behavior |
|---|---|---|
| `addgroup` | `{"graph":String,"name":String,"nodes":[String],"metadata":{"description":String,"icon"?:String}}` | "Create group": builds a composite block type from the selected nodes (`03-block-types.md` §5.2), registers it under `name`, and persists it. Broadcasts `graph:addgroup` (same shape) to subscribers of the graph and a `component:component` for the new type to every authenticated connection. The selected nodes are left unchanged ("save as component"). Refused if `name` is already a registered block type. |
| `removegroup` | `{"graph":String,"name":String}` | Unregisters the type and deletes its file. Refused while any graph or other composite type still uses it. `graph:removegroup` broadcast `{"graph","name"}`. |
| `renamegroup` | `{"graph":String,"from":String,"to":String}` | Renames the type. Refused while the type is in use (instances refer to their type by name). `graph:renamegroup` broadcast `{"graph","from","to"}`. |
| `changegroup` | `{"graph":String,"name":String,"metadata":{"description":String,"icon"?:String}}` | Updates the type's description/icon. `graph:changegroup` broadcast `{"graph","name","metadata":{...}}`, followed by a refreshed `component:component`. |

The model side of these commands exists (plan task 21, `src/blocktypes/composite_types.h`, `src/app/type_files.h`): building a type from a selection (`03-block-types.md` §5.2), registering, unregistering and renaming with the refusals and provisional messages of `03-block-types.md` §5.1 (refused while in use), changing the description and icon (allowed while in use), saving and deleting the type file (`05-app-lifecycle-persistence.md` §2.3). The commands themselves still answer the generic error (§3.4 "Groups") until they are implemented (plan task 21b).

### 3.5 `component`

| command | payload | reply |
|---|---|---|
| `list` | `{}` | One `component:component` per registered block type, then one `component:componentsready`. |
| `getsource` | `{"name":String}` | Replies with `component:source` `{"name":<name>,"language":"json","library":"hakkas","code":<code>,"tests":""}`. For a composite type, `code` is its authoring JSON (`03-block-types.md` §5.1) serialized as a string. For a built-in type, `code` is `""`. Unknown name → `network:error` `"Unknown component: <name>"`. |
| other | — | generic error |

**`component:component`** push, one per registered block type:
```json
{"protocol":"component","command":"component","payload":{
  "name":"<e.g. math/Add2>","description":"<string>","subgraph":<bool>,"icon":"<icon name>",
  "inPorts":[{"id":"<port>","description":"<string>","type":"<number|bool|string>","addressable":<bool>}, ...],
  "outPorts":[ same shape ]
}}
```
`subgraph` is `true` for composite (group) block types, `false` otherwise.

Decided (plan task 17): `getsource` without a string `name` is `"Invalid field: name"` (§1.2). Decided (plan task 21): composite types are listed by `component:list` like any type, in registration order (the built-ins, then the composites, `01-architecture-overview.md` §2.3), with `subgraph: true` and their exposed ports (a port's description, type and `addressable` from the authoring JSON, else from the mapped inner port, `03-block-types.md` §5.1); `getsource`'s `code` for a composite is its authoring JSON in canonical form (`03-block-types.md` §5.1), the same text its type file holds. Composite types loaded at startup are not broadcast: no client is connected yet (`05-app-lifecycle-persistence.md` §1.4). The `component:component` broadcast of a new or changed type belongs to the group commands (§3.4.2), which are not implemented.

**`component:componentsready`**: bare-integer payload, see §1.

### 3.6 `trace`

Tracing isn't an active feature of this system. Every command is accepted and acknowledged as a no-op, using the reply shapes from the upstream protocol (`fbp-protocol.html`, "Trace protocol"):
- `trace:start {"graph","buffersize"?}` → `trace:start {"graph":<id>}`
- `trace:stop {"graph"}` → `trace:stop {"graph":<id>}`
- `trace:clear {"graph"}` → `trace:clear {"graph":<id>}`
- `trace:dump {"graph"}` → `trace:dump {"graph":<id>,"type":"flowtrace.json","flowtrace":""}`

Build out further only if a real tracing/debugging feature is wanted later.

Decided (plan task 17): `graph` must be a string (else `"Invalid field: graph"`, §1.2) and is echoed as given, whether or not such a graph exists; `buffersize` is ignored.

## 4. Subscription model

### 4.1 Connection lifecycle and graph-event scoping

A connection starts receiving event delivery (graph-change broadcasts, log/output mirroring) once it has logged in (§2.5 item 6). By default it receives events for every graph in the system. Issuing `graph:connect {"id":X}` scopes graph-specific events to graph `X` only: added/renamed/changed/removed block; added/removed connection; added/removed/renamed in/outport; added/removed initial; groups; started; stopped. System-wide events always reach every authenticated connection regardless of which graph it's scoped to: errors, `runtime:new_graph`, `runtime:delete_graph`, `network:status`, `component:component`. Issuing `graph:connect` again with a different id re-scopes the same connection. `graph:rename` is graph-specific (decided, plan task 7): it reaches the subscribers of that graph; others learn the new name from the next `network:status`.

Decided (plan task 17): a connection becomes a subscriber when its login succeeds and stops being one at logout, session expiry or close; every login starts unscoped (every graph), also a login after a logout. The scope is the connection's, set by `graph:connect` together with its snapshot (plan task 18, §4.1.1); a refused `graph:connect` leaves it as it was. Each event is turned into its wire message once and the same text is sent to every subscribed connection in subscription order.

### 4.1.1 `graph:connect` reply

On `graph:connect`, send the complete current graph structure to the requester as a single, immediate sequence, all in one atomic reply rather than staged across a delay. Because the engine is single-threaded, no other event can be interleaved into the snapshot. Every event for this graph broadcast after the snapshot applies on top of it, so the client's view is exactly the engine's model from then on (§1.1). The client clears its view of the graph on `graph:clear` and builds it only from the snapshot that follows:
1. `graph:clear` (`{"id","name","main":true,"description","icon"}`)
2. one `graph:addnode` per block (with its full metadata)
3. one `graph:addinport`/`graph:addoutport` per exported port
4. one `graph:addinitial` per input that has a literal wired to it
5. one `graph:addedge` per link (with its metadata)

Implementation decisions (plan task 18; `src/protocol/dispatcher.c`):
- **Order within the steps**: blocks in creation order; inports, then outports, each in creation order; initials and edges per block (creation order) and input (port order), as in the graph file (`05-app-lifecycle-persistence.md` §2.1). A placeholder block is sent with its original `component`; the retained entries that reference placeholders are never sent (§3.4).
- **Fitting the buffer** (§0 "Outgoing buffer", §1.2 "Bursts", 10 bytes of framing counted per message): if the snapshot fits the connection's free outgoing space, the connection is re-scoped and the snapshot sent at the same instant. If it does not fit while the buffer still holds earlier messages, the `graph:connect` is deferred until the buffer has drained (`on_drained`); the snapshot is then built from the graph as it is at that moment and the connection re-scoped at that same instant. Until then the connection keeps its previous scope, so events keep arriving under it — all of them before the snapshot's `graph:clear`, which discards them — and no event of the new graph can come between the re-scope and the snapshot. A newer `graph:connect` replaces a deferred one; a logout or session expiry drops it; a graph deleted meanwhile is answered `"Unknown graph: <id>"` at the drain.
- **Too large** (user decision): a snapshot that does not fit the outgoing buffer even when it is empty is refused with `network:error` `"Graph too large to send: <id>"` (with `"graph"`) to the requester only. The connection stays open and is **not** re-scoped: closing it would only start an endless reconnect cycle. With the default `WS_OUT_CAP` this takes a graph of several hundred blocks, links and initials (§0); the remedy is a larger `WS_OUT_CAP` for that build.
- **Refusals** (`"Invalid field: id"`, `"Invalid id"`, `"Unknown graph: <id>"`) leave the scope as it was.

### 4.2 Link-value subscriptions (`network:edges`)

Request: `{"graph":"...","edges":[{"src":{"node","port"},"tgt":{"node","port"}}, ...]}`. Each pair becomes a live subscription on the destination input. The server immediately sends one `network:data` with the input's current value, then one more on every subsequent change of that input. (The subscription only reads the input; it doesn't trigger a compute step.) A pair whose `tgt` doesn't exist is ignored.

A connection has at most one active set of link subscriptions: a new `network:edges` request replaces the previous set entirely rather than merging with it. Subscriptions are torn down by sending a replacement request (an empty `edges` array clears them), or implicitly on disconnect or logout. Reply to the requester only: `network:edges` echoing the request payload (a plain acknowledgement).

Implementation decisions (plan task 18; `src/protocol/dispatcher.c`, the input watches of `01-architecture-overview.md` §2.2):
- **Request**: `graph` must be a string and `edges` an array of `{"src":{"node","port"},"tgt":{"node","port"}}` with string members, else `"Invalid field: graph"` / `"Invalid field: edges"`, and the previous set stays. An invalid or unknown `graph` is not an error: every pair is ignored (like a nonexistent `tgt`), so the set becomes empty, and the request is echoed as usual.
- **Existing `tgt`**: an input port of a block of the graph; an output, an unknown node or port and a placeholder's port are ignored. The same input may be subscribed more than once. `src` is not checked against the input's actual source.
- **Order**: the echo (the request payload as received, every member included) goes first, then one `network:data` per subscription, in request order, with the input's current value.
- **`network:data`**: `graph` is the request's graph id, `src` and `tgt` the request's pair as given (a later `renamenode` does not change them; the client sends a new request), `data` the value stringified (`linkable_stringify`, as initials, §3.4).
- **Changes**: every update of the input (`03-block-types.md` §2) sends one: a value arriving over the link (when the queue is drained) and a structural change on the input itself (an initial added, replaced or removed, its edge removed) at once.
- **Back-pressure**: link values are a stream of current values, not events: a `network:data` that does not fit the free outgoing space above the reserve (below) is not sent and does not close the connection; once the buffer has drained (`on_drained`), the input's value at that moment is sent instead. Intermediate values are skipped, the client always ends up with the current one.
- **Reserve** (decided after plan task 20: a probe showed a fast stream filling the buffer to within one value of full, so the client's next reply or the next event closed the connection): replies and events are not lossy, and one that does not fit closes the connection (§0), so values must never take the space they need. A `network:data` is sent only if at least the **reserve** stays free after it; otherwise it is coalesced as above. The reserve is a quarter of the buffer — the largest message the empty buffer takes divided by `PROTO_DATA_RESERVE_DIV` (4, a per-build override; `src/protocol/dispatcher.h`): 16 KiB of the default 64 KiB `WS_OUT_CAP`, 4 KiB of a 16 KiB MCU buffer. Replies and events may use it. A fraction rather than a fixed size, because it scales with the per-build `WS_OUT_CAP`: a quarter holds dozens of typical replies and events (100–300 bytes each) or the largest single non-burst message in practice (a `network:edges` echo of a large request, a `"Badly formatted request"` error of up to ~1.6 KiB), while three quarters stay for the value stream's throughput. It bounds what a client that has stopped reading can still be sent, as without a stream; it cannot make an unbounded number of replies fit. A value whose message would not leave the reserve free even in an empty buffer is dropped. The log mirror's lines (05 §4) follow the same rule (dropped rather than coalesced). While a burst is deferred, values are coalesced however much space is free (§1.2 "Bursts"); at the drain the burst goes first.
- **Teardown**: a replacement, an empty array, logout, session expiry (and nothing is sent from the session's deadline on), the connection's close; a subscription ends silently with its block (`graph:removenode`, `runtime:delete_graph`). Subscriptions are independent of the `graph:connect` scope, and reach only authenticated connections (§2.5 item 6).

## 5. Relationship to the FBP network protocol standard

This protocol is a dialect of the standard FBP network protocol (`fbp-protocol.html` — the one NoFlo/Flowhub and similar tools implement), not a literal implementation of it. The notable departures:

1. **Top-level `secret` field** on every client→server message, rather than the standard's `requestId`/`responseTo` request-correlation fields. There is no request/response correlation in this protocol at all: replies are matched to requests by protocol+command and client-side context, not by an id round-trip, and clients should not expect or require one.
2. A stateful **`environment:challenge`/`login`/`logout`** handshake (§2.3) mints the per-connection secret, rather than treating `secret` as a static pre-shared credential sent on every message.
3. **`graph:list`** doesn't return a bulk list in one message — it triggers a `network:status` broadcast per graph.
4. **`graph:connect`** is not a standard command; it combines "scope event delivery to this graph" with "send its full current structure," as one atomic reply (§4.1.1).
5. **Capabilities are per role** (§2.4), using the standard capability names. They are enforced per command, but in four coarse levels rather than per-user custom sets.
6. **All errors are reported as `network:error`** — there's no per-sub-protocol error command (`graph:error`, `component:error`, etc.).
7. **`network:icon`/`connect`/`disconnect`/`begingroup`/`endgroup`** (standard FBP execution-tracing events) are not part of this protocol.
8. **Values are transmitted as strings** on `network:data` and in initial-value broadcasts, not as native JSON types — a receiver re-parses according to the port's declared type (§3.4).
9. **Groups** (§3.4.2) define reusable block types instead of being purely visual groupings.
