# 02 — WebSocket Wire Protocol

This is the protocol the browser frontend speaks. Most of it is a live contract: the frontend already implements it and should not need to change. A few areas — authentication (which changes the login messages, so the frontend's login code must be updated) and the group/composite-block commands — have no real frontend implementation yet, so they're specified as a clean design rather than constrained by anything existing; those are marked explicitly. Everywhere else, treat this document as the actual wire format, not a target to approximate.

## 0. Transport

Two TCP ports, both set in the configuration file (`05-app-lifecycle-persistence.md` §1.1):
- `port` (default `8080`) — the web server serving the frontend's static assets only (`05-app-lifecycle-persistence.md` §3).
- `port+1` — the FBP WebSocket, at `ws://<host>:<port+1>/` (`wss://` when TLS is enabled, §2.5).

The user enters `http://<host>:<port>/` in the browser; the frontend derives the WebSocket port as the page's own port + 1, so the WebSocket port never has to be entered separately.

One WebSocket per browser tab. Text frames only, one JSON object per frame, no batching. The server accepts at most `max_connections` concurrent WebSocket connections (configuration file) and a maximum frame size of `max_message_bytes`; an upgrade beyond the connection limit is refused with HTTP 503, and an oversized frame closes the connection with WebSocket close code 1009. The upgrade request's `Origin` header must match the host and web port the frontend was served from (or be one of the configured `allowed_origins`); otherwise the upgrade is refused with HTTP 403. This blocks cross-site WebSocket hijacking from other web pages open in the same browser.

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
- The same rule applies outside `graph:*`: `runtime:new_graph`/`delete_graph`, `network:start`/`stop`, and group/type changes are all reflected only through their broadcast events. Requests that change nothing — `graph:connect`, `network:edges`, `component:list`, `getstatus`, `trace:*` — get a direct reply to the sender instead.

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
- **Unknown users get a normal-looking reply**: the server returns `salt = HMAC(server_secret, user)[0..16]` and the default iteration count, where `server_secret` is a random value generated once and stored with the configuration. The reply for an unknown user is indistinguishable from a real one, so user names can't be probed.

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

### 2.5 Sessions and secrets

1. **Secret format**: 128 bits from the PAL's cryptographic random source, base64url-encoded (22 chars).
2. **Binding**: a secret is valid only on the connection it was issued to. A secret presented on any other connection is invalid. Closing the connection destroys the session.
3. **Lifetime**: sessions expire after `auth.idle_timeout_s` without a client message (default 1800 s) and after `auth.max_lifetime_s` in total (default 43200 s). On expiry the server sends `{"protocol":"environment","command":"logout"}`, stops event delivery, and the connection returns to `UNAUTHENTICATED` (the socket stays open so the client can log in again).
4. **Logout**: `environment:logout` invalidates the connection's session and returns it to `UNAUTHENTICATED`.
5. **Secret gating**: every message except `environment:challenge`, `environment:login`, and `runtime:getruntime` must carry the connection's current secret. An invalid, expired, or missing secret → `network:error` `"Not authenticated"`. The submitted value is never echoed back.
6. **Event delivery requires authentication**: graph-change broadcasts, `network:data`, and log mirroring are only delivered to `AUTHENTICATED` connections. There is no passive, pre-login event stream.
7. **Rate limiting**: a connection is closed after 3 failed logins. Per source address, consecutive failures trigger exponential back-off: after the 5th failure, further challenges from that address are delayed 1 s, doubling up to 60 s, and the counter resets after a success or 15 min of quiet. The table is bounded (default 16 addresses, least-recently-used eviction) so memory use is fixed on the MCU.
8. **Logging**: never log secrets, proofs, nonces, salts, or passwords. Log the fact of a login success or failure with the user name and source address only.
9. **TLS (optional, default off)**: Zephyr ships TLS through its mbedTLS integration (TLS socket options in its socket API), and on POSIX OpenSSL/mbedTLS is available, so no custom TLS is needed. When `tls.enabled` is set, both ports serve TLS (`https://` / `wss://`) using a certificate and key from the configuration directory. It is off by default because of the RAM cost on the MCU and the browser warnings a self-signed LAN certificate causes. The handshake above is designed to stay safe without it. Without TLS, an eavesdropper can still see the traffic after login (graph contents, values), but can't recover the password or reuse the session.
10. **Testability**: the authorization component is a small module behind an interface (`challenge`, `verify_proof`, `issue_secret`, `validate_secret`, `revoke_secret`, `capabilities_for`), so protocol-handler tests can substitute a fake implementation. Integration tests that need the real flow use a deliberately provisioned test users file, never a credential compiled into the build.

## 3. Protocol namespaces

Every command below requires an authenticated connection and the capability listed in §2.4, unless noted otherwise.

### 3.1 `environment` — `challenge`, `login`, `logout` (§2). Not secret-gated. Anything else → generic error.

### 3.2 `runtime`

| command | request payload | behavior |
|---|---|---|
| `getruntime` | `{}` | **Allowed before login.** Before login: sends only the `runtime:runtime` push (§3.2.1) with `"capabilities":[]`, so a client can discover the runtime and see that it must log in. After login: sends `runtime:runtime` with the session's capabilities; one `component:component` per registered block type; one `component:componentsready` with the count; and triggers a `network:status` broadcast for every existing graph. |
| `new_graph` | `{"id"?,"name"?,"description"?,"icon"?}` all optional (`id`→fresh random UUID v4, lowercase, if absent, `name`→`"<<new graph>>"`, others→`""`) | Creates the graph (stopped, empty); acknowledgement arrives via the `runtime:new_graph` broadcast (§3.2.2) to every subscriber. Not persisted until `network:persist`. |
| `delete_graph` | `{"id","name"}` both required | Stops and deletes the graph **and removes its persisted file** (`05-app-lifecycle-persistence.md` §2.3); acknowledgement arrives via the `runtime:delete_graph` broadcast (§3.2.3). `name` must match the graph's current name (a guard against deleting the wrong graph). A started graph is stopped first, and its `network:stopped` is broadcast before the `runtime:delete_graph` (§1.1, compound effects). |
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
| `addnode` | `{"id","graph","component","metadata"?:{"x","y","change_threshold"?}}` (`id`/`graph`/`component` required) | Creates the block (started immediately if the graph is started). `graph:addnode` broadcast `{"graph","component","id","metadata":{"x","y"[,"change_threshold"]}}`. Missing `x`/`y` default to `50`. |
| `removenode` | `{"id","graph"}` | Removes the block; every input it fed reverts to its default (`01-architecture-overview.md` §2.2). Broadcasts `graph:removeedge`/`graph:removeinitial` for each attached connection first, then `graph:removenode` `{"id","graph"}`. |
| `renamenode` | `{"from","to","graph"}` | `graph:renamenode` broadcast `{"graph","from","to"}`. Links are unaffected. |
| `changenode` | `{"id","metadata":{"x"?,"y"?,"change_threshold"?},"graph"}` | Merges the given metadata keys into the node's metadata; `"change_threshold": null` removes the instance override (`01-architecture-overview.md` §3.3). `graph:changenode` broadcast `{"id","graph","metadata":{<full resulting metadata>}}`. |
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

Graph-level inports/outports define the external ports a graph has when it is used as a group/composite block type (`01-architecture-overview.md` §2.2, `03-block-types.md` §5). They have no dataflow effect in a top-level graph.

Graph ports (decided, plan task 7): inports and outports are separate namespaces (an inport and an outport may share a public name). Several ports may map to the same inner port. A port follows its node: `renamenode` changes the node every port mapped to it refers to, and needs no port event (the `graph:renamenode` implies it). `removenode` removes every port mapped to the node, broadcasting a `graph:removeinport`/`graph:removeoutport` for each (inports, then outports, each in creation order) after the removed edges and initials and before the `graph:removenode`.

**Initial value encoding**: the `data` of an initial is accepted as a JSON string, number, or boolean. A string is parsed according to the target port's declared type (`"number"`: as a number, or as a `Metric` if it has the form `"<number> <unit>"`; `"bool"`: `to_bool`; `"string"`: as-is). Details (`01-architecture-overview.md` §2.1): a `"number"` string that is wholly a number becomes an f64 (never an i64 — `"number"` ports default to `0.0`, and JSON numbers are doubles too); otherwise it is split on its first space, and if the part before is a number and the rest (the unit, kept verbatim) is non-empty, it becomes a `Metric` with category unclassified; any other text gives `0.0`, the same as `to_f64` of that string. A port type other than `"bool"`/`"string"` parses like `"number"`, matching `default_value`. The `graph:addinitial` event carries `data` as a string, matching what the existing frontend sends. The persisted file stores it as a native JSON value (`05-app-lifecycle-persistence.md` §2).

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

> **Provisional.** No frontend implementation exists yet and the exact mechanism isn't settled (`00-README.md`, open question 4). The commands below reuse the standard FBP group commands with the meaning this system gives a group: a **reusable user-defined block type** built from a selection of blocks (`03-block-types.md` §5).

| command | request payload | behavior |
|---|---|---|
| `addgroup` | `{"graph":String,"name":String,"nodes":[String],"metadata":{"description":String,"icon"?:String}}` | "Create group": builds a composite block type from the selected nodes (`03-block-types.md` §5.2), registers it under `name`, and persists it. Broadcasts `graph:addgroup` (same shape) to subscribers of the graph and a `component:component` for the new type to every authenticated connection. The selected nodes are left unchanged; whether they should be replaced by an instance of the new type is open. Refused if `name` is already a registered block type. |
| `removegroup` | `{"graph":String,"name":String}` | Unregisters the type and deletes its file. Refused while any graph or other composite type still uses it. `graph:removegroup` broadcast `{"graph","name"}`. |
| `renamegroup` | `{"graph":String,"from":String,"to":String}` | Renames the type. Refused while the type is in use (instances refer to their type by name). `graph:renamegroup` broadcast `{"graph","from","to"}`. |
| `changegroup` | `{"graph":String,"name":String,"metadata":{"description":String,"icon"?:String}}` | Updates the type's description/icon. `graph:changegroup` broadcast `{"graph","name","metadata":{...}}`, followed by a refreshed `component:component`. |

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

**`component:componentsready`**: bare-integer payload, see §1.

### 3.6 `trace`

Tracing isn't an active feature of this system. Every command is accepted and acknowledged as a no-op, using the reply shapes from the upstream protocol (`fbp-protocol.html`, "Trace protocol"):
- `trace:start {"graph","buffersize"?}` → `trace:start {"graph":<id>}`
- `trace:stop {"graph"}` → `trace:stop {"graph":<id>}`
- `trace:clear {"graph"}` → `trace:clear {"graph":<id>}`
- `trace:dump {"graph"}` → `trace:dump {"graph":<id>,"type":"flowtrace.json","flowtrace":""}`

Build out further only if a real tracing/debugging feature is wanted later.

## 4. Subscription model

### 4.1 Connection lifecycle and graph-event scoping

A connection starts receiving event delivery (graph-change broadcasts, log/output mirroring) once it has logged in (§2.5 item 6). By default it receives events for every graph in the system. Issuing `graph:connect {"id":X}` scopes graph-specific events to graph `X` only: added/renamed/changed/removed block; added/removed connection; added/removed/renamed in/outport; added/removed initial; groups; started; stopped. System-wide events always reach every authenticated connection regardless of which graph it's scoped to: errors, `runtime:new_graph`, `runtime:delete_graph`, `network:status`, `component:component`. Issuing `graph:connect` again with a different id re-scopes the same connection. `graph:rename` is graph-specific (decided, plan task 7): it reaches the subscribers of that graph; others learn the new name from the next `network:status`.

### 4.1.1 `graph:connect` reply

On `graph:connect`, send the complete current graph structure to the requester as a single, immediate sequence, all in one atomic reply rather than staged across a delay. Because the engine is single-threaded, no other event can be interleaved into the snapshot. Every event for this graph broadcast after the snapshot applies on top of it, so the client's view is exactly the engine's model from then on (§1.1). The client clears its view of the graph on `graph:clear` and builds it only from the snapshot that follows:
1. `graph:clear` (`{"id","name","main":true,"description","icon"}`)
2. one `graph:addnode` per block (with its full metadata)
3. one `graph:addinport`/`graph:addoutport` per exported port
4. one `graph:addinitial` per input that has a literal wired to it
5. one `graph:addedge` per link (with its metadata)

### 4.2 Link-value subscriptions (`network:edges`)

Request: `{"graph":"...","edges":[{"src":{"node","port"},"tgt":{"node","port"}}, ...]}`. Each pair becomes a live subscription on the destination input. The server immediately sends one `network:data` with the input's current value, then one more on every subsequent change of that input. (The subscription only reads the input; it doesn't trigger a compute step.) A pair whose `tgt` doesn't exist is ignored.

A connection has at most one active set of link subscriptions: a new `network:edges` request replaces the previous set entirely rather than merging with it. Subscriptions are torn down by sending a replacement request (an empty `edges` array clears them), or implicitly on disconnect or logout. Reply to the requester only: `network:edges` echoing the request payload (a plain acknowledgement).

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
