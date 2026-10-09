# 05 — Startup, Configuration, Persistence, Web Server, Logging

## 1. Configuration and startup

### 1.1 Configuration file

Both build trees read one JSON configuration file, `hakkas.json`:
- POSIX: `<confdir>/hakkas.json` (`--confdir`, default `/etc/hakkas`);
- Zephyr: `/lfs/config/hakkas.json` on the LittleFS partition.

Every key is optional; a missing key takes the default below, and a missing file means "all defaults". On the POSIX build, command-line options (§1.2) override the file.

| Key | Default | Meaning |
|---|---|---|
| `engine_id` | *(unset → a random UUID each boot)* | Engine identity, reported as `id` in `runtime:runtime` (`02-wire-protocol.md` §3.2) and in mDNS TXT records. |
| `label` | `"Hakkas"` | `label` in `runtime:runtime`. |
| `host` | `"0.0.0.0"` | Address both listeners bind to. |
| `port` | `8080` | Web server port (static assets). The FBP WebSocket listens on `port + 1` (`02-wire-protocol.md` §0). |
| `webdir` | POSIX: `<cwd>/frontend/src`; Zephyr: `/lfs/web` | Location of the frontend's static assets. |
| `datadir` | POSIX: `<basedir>`; Zephyr: `/lfs` | Parent directory of `graphs/` and `types/` (§2.3). |
| `max_connections` | `4` | Maximum concurrent WebSocket connections. |
| `max_message_bytes` | `16384` | Maximum WebSocket message size. |
| `allowed_origins` | `[]` | Extra `Origin` values accepted on the WebSocket upgrade, besides the device's own (`02-wire-protocol.md` §0). |
| `change_threshold` | `0.05` | Engine-wide default change threshold (`01-architecture-overview.md` §3.3). |
| `log.level` | `"warn"` | `fine`, `info`, `warn`, or `error` (§4). |
| `log.mirror` | `false` | Mirror log output to authenticated clients as `network:output`/`network:error` (§4). |
| `auth.users_file` | POSIX: `<confdir>/users.json`; Zephyr: `/lfs/config/users.json` | Credential store (`02-wire-protocol.md` §2.2). |
| `auth.idle_timeout_s` | `1800` | Session idle timeout (`02-wire-protocol.md` §2.5). |
| `auth.max_lifetime_s` | `43200` | Session absolute lifetime. |
| `auth.server_secret` | *(generated on first boot and written back to the file)* | Random key used to fake challenges for unknown users (`02-wire-protocol.md` §2.3). |
| `tls.enabled`, `tls.cert`, `tls.key` | `false`, —, — | Optional TLS on both ports (`02-wire-protocol.md` §2.5 item 9). |
| `net.hostname` | `"hakkas-<last 3 MAC bytes in hex>"` | mDNS hostname (Zephyr; `04-drivers.md` §6). |
| `net.dhcp`, `net.address`, `net.netmask`, `net.gateway` | `true`, —, —, — | IPv4 configuration (Zephyr only; the POSIX build uses the host OS's network configuration). |

The file is written (when `auth.server_secret` is generated) using the same atomic-write procedure as every other persisted file (§2.3).

### 1.2 POSIX build tree (Raspberry Pi, x86_64 development)

A command-line tree configures and starts the runtime:

- Global options: `--fine`/`--info`/`--warn` (log verbosity, most-to-least verbose; overrides `log.level`), `--basedir <dir>` (default `.`), `--confdir <dir>` (default `/etc/hakkas`).
- `list types` — print every registered block type name.
- `describe type <typename>` — print one block type's JSON descriptor.
- `describe topology <filename>` — load a graph file standalone and print its JSON description.
- `user add|passwd|del|list ...` — manage the credential store (`02-wire-protocol.md` §2.2).
- `run [options] process <filenames>` — start the engine and load the given colon-separated list of graph files, in order.
- `run [options] daemon` — start the engine and load every graph in `<datadir>/graphs/`.
- `run`'s own options override the corresponding configuration-file keys: `--id <id>` (`engine_id`), `--webdir <dir>`, `--host <addr>`, `--port <n>`, `--users <file>` (`auth.users_file`).

### 1.3 Zephyr build tree (MCU targets)

There is no argv to parse at boot. Configuration comes from Kconfig (compile-time limits and features), devicetree (board topology), and the configuration file on LittleFS (§1.1). The device always behaves like `run daemon`. An equivalent of the `list types`/`describe type`/`describe topology`/`user …` commands is exposed through Zephyr's shell subsystem over a console/USB-CDC backend, for interactive diagnostics and for provisioning users.

### 1.4 Startup sequence (both build trees)

1. Read the configuration file and apply command-line overrides (POSIX) → initialize logging and file locations. On Zephyr, first mount LittleFS.
2. Initialize the PAL: clock, random source, storage; on Zephyr also bring up networking (`04-drivers.md` §6). Delete any leftover `*.tmp` files from an interrupted write (§2.3).
3. Load the credential store and initialize the authorization component (`02-wire-protocol.md` §2).
4. Register every built-in block type (`03-block-types.md`).
5. Initialize the IO provider: discover and configure hardware modules on Zephyr, or set up simulated channels on POSIX (`04-drivers.md` §0–§1).
6. Load composite block types from `<datadir>/types/` in dependency order: repeatedly register every type whose inner types are all registered, until no more can be registered. Anything left over (an unknown inner type, or a cycle) is logged as an error and skipped.
7. Load the configured graph files (POSIX `process`) or every graph in `<datadir>/graphs/` (POSIX `daemon`, Zephyr). Start each graph whose persisted `started` flag is `true` (§2.1).
8. Open the web server on `port` and the FBP WebSocket listener on `port + 1`.
9. Enter the engine loop (`01-architecture-overview.md` §3.2).

## 2. Persistence

### 2.1 Graph file format

One canonical JSON schema is used for both loading and saving a graph. Saving a loaded graph reproduces the same graph: the same blocks, connections, metadata, and ports.

```json
{
  "id": "...", "name": "...", "description": "...", "icon": "...",
  "started": true,
  "blocks": {
    "<instance-name>": { "component": "<type-name>", "metadata": { "x": 0, "y": 0, "change_threshold": 0.1 } }
  },
  "connections": [
    { "src": {"node":"<block>","port":"<port>"}, "tgt": {"node":"<block>","port":"<port>"}, "metadata": { "route": 3 } },
    { "src": {"data": <value>}, "tgt": {"node":"<block>","port":"<port>"} }
  ],
  "inports":  [ { "name": "...", "node": "...", "port": "..." } ],
  "outports": [ { "name": "...", "node": "...", "port": "..." } ]
}
```

- `started` is the graph's started/stopped state when it was persisted; it is restored at boot (`00-README.md`, open question 12). It defaults to `false` if absent.
- `blocks` is an object keyed by instance name; each value's `component` names a registered block type. `metadata.x`/`metadata.y` are the canvas position (default `50,50`). `metadata.change_threshold` is the optional per-instance change threshold override (`01-architecture-overview.md` §3.3). Any other metadata keys are preserved as-is.
- `connections` entries are either a normal edge (`src`/`tgt` both reference `{node, port}`, with optional opaque UI `metadata`) or an initial (`src` is `{"data": <value>}`, stored as a native JSON string, number, or boolean — `02-wire-protocol.md` §3.4).
- `inports`/`outports` declare the graph-level named ports that a group built from this graph would expose (`01-architecture-overview.md` §2.2).
- Groups are not stored in the graph file: a group is a block type, stored in `types/` (§2.3, `03-block-types.md` §5.1).

### 2.2 Loading rules

- Each block's `component` is resolved against the block type registry. An unresolvable type name produces a **placeholder block**: it logs a warning, has no ports, and takes part in no dataflow. It keeps its original `component` name, metadata, and every connection that referenced it, and writes them back unchanged on save, so loading a graph on a system that lacks a type never destroys data.
- A connection that violates the graph's invariants (unknown node or port, fan-in) is logged and skipped, exactly as the equivalent wire command would be refused (`02-wire-protocol.md` §3.4.1).
- Initials are applied as their connections are loaded; the graph starts (if `started`) only after the whole file is loaded.

### 2.3 Storage layout, file naming, atomic writes

```
<datadir>/
  graphs/<encoded graph id>.json
  types/<encoded type name>.json     (composite / group block types, 03-block-types.md §5.1)
```

`<datadir>` is a plain directory on POSIX (default `<basedir>`) and the LittleFS mount on Zephyr (default `/lfs`). `graphs/` and `types/` share the same parent directory. The configuration and users files live in the configuration directory (§1.1).

**File naming**: users never see these files, so names only need to be safe and reversible:
- Graph ids and type names are limited to 64 bytes (`02-wire-protocol.md` §3.4.1).
- They are encoded by keeping the bytes `A–Z a–z 0–9 _ -` and writing every other byte as `%XX` (uppercase hex), then appending `.json`. For example, `math/MyBlock` → `math%2FMyBlock.json`, and a UUID is unchanged.
- The longest possible name (3 × 64 + 5 bytes) fits LittleFS's default 255-byte name limit.
- The id/name inside the file is authoritative; the file name is only an index.

**Atomic writes** — every write (graphs, types, configuration, users) uses the same procedure, so a power loss mid-write leaves either the old or the new file, never a torn one:
1. Write the complete content to `<name>.tmp` in the same directory.
2. Flush it to stable storage and close it (`fs_sync` on Zephyr; `fsync` on POSIX).
3. Rename `<name>.tmp` over `<name>`. The rename is atomic on LittleFS and on POSIX filesystems. On POSIX, also `fsync` the directory.
4. At startup, delete any leftover `*.tmp` (§1.4) in every directory the engine writes to: `<datadir>/graphs/`, `<datadir>/types/`, and the directories of the configuration and users files.

This procedure is the PAL operation `pal_storage_write` (`01-architecture-overview.md` §0.1), and step 4 is `pal_storage_remove_tmp`. The temporary file is the full file name plus `.tmp` (`x.json` → `x.json.tmp`, at most 201 bytes for an encoded name, within LittleFS's 255). On POSIX every system call is retried on `EINTR`, partial writes are continued, a failure before the rename removes the `.tmp` file and leaves the old file untouched, and files are created with mode `0600` (the same procedure writes the users file). A filesystem that cannot `fsync` a directory (`EINVAL`) is tolerated.

**Deleting**: `runtime:delete_graph` removes the graph's file; removing a group removes its type file (`02-wire-protocol.md` §3.2, §3.4.2). Renaming a graph changes only its `name`, never its `id`, so the file name stays the same.

## 3. Web server

The web server listens on `port` and serves only the frontend's static assets (HTML/JS/CSS bundle) from `webdir`. It does not handle the FBP protocol, which has its own WebSocket listener on `port + 1` (`02-wire-protocol.md` §0).
- Only `GET` is accepted. Unmatched paths fall back to the SPA's entry document.
- The web server requires no authentication: the assets contain no secrets, and authentication happens on the WebSocket (`02-wire-protocol.md` §2).
- To save flash, assets may be stored pre-compressed: if `<file>.gz` exists and the request's `Accept-Encoding` includes `gzip`, serve it with `Content-Encoding: gzip`.

On the Zephyr build, assets live in LittleFS (`/lfs/web`), so the frontend can be updated without reflashing the firmware. Serving the bundle from flash carries a real storage-size budget — confirm the compressed bundle's size against the target MCU's flash budget during implementation.

## 4. Logging

Four severity levels, `fine < info < warn < error`, with a single verbosity threshold (`log.level`, §1.1): a message is emitted only if its level is at or above the configured threshold. Log lines are tagged with their source location (file/line) for traceability. Remote log mirroring relays log output to authenticated WebSocket clients as `network:output` (`fine`/`info`/`warn`) or `network:error` (`error`), per `02-wire-protocol.md` §3.3. It is enabled with `log.mirror` and is off by default. Secrets and credentials are never logged (`02-wire-protocol.md` §2.5 item 8).

- **Line format**: `<level> <file>:<line>: <message>` — the level name as in `log.level`, the source file's base name, no timestamp (`01-architecture-overview.md` §3.4 has no wall clock yet; a sink may add its own). A line is formatted into a fixed 256-byte buffer, so it holds at most 255 characters; a longer line is cut there with its last three characters replaced by `...`. No heap allocation per log call.
- **Sinks**: the portable core only formats; each line goes, with its level, to every registered sink (up to 4). The platform registers its console (stderr on POSIX, the Zephyr console later); the protocol layer registers the mirror when `log.mirror` is on and uses the level to choose `network:output` or `network:error`. With no sink registered, output is discarded.
- **Engine thread only**: logging is not locked (`01-architecture-overview.md` §3.2). IO workers and ISRs never log; they enqueue work and the engine-side handler logs. (Timers fire on the engine thread, `01-architecture-overview.md` §3.2 item 7.) A log call made from inside a sink is dropped.

## 5. Testing

Because the authorization component, the graph engine, the protocol dispatcher, and the IO provider are each separable modules behind interfaces (`01-architecture-overview.md` §0, §6; `02-wire-protocol.md` §2.5 item 10; `04-drivers.md` §0), unit tests can exercise protocol command handling against a fake authorizer, an in-memory graph, and the simulated IO provider, without real credential hashing or hardware access. Integration tests that exercise the real WebSocket/FBP protocol end-to-end should use the `02-wire-protocol.md` message catalog directly as the assertion basis: each documented request/response pair, and each refusal case in `02-wire-protocol.md` §3.4.1, is a concrete test case. Multi-client tests should check the command → event rule (`02-wire-protocol.md` §1.1): with two connections on the same graph, a command from one produces the identical event sequence on both, and a refused command produces no event on either.
