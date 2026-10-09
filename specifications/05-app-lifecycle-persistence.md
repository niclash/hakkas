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

Implementation decisions (plan task 11, `src/system/config.h`):
- **Nested keys**: a key `<section>.<key>` of the table is the member `<key>` of the object `<section>` (`log`, `auth`, `tls`, `net`), e.g. `{"log": {"level": "info"}}`. A top-level member literally named `"log.level"` is an unknown key.
- **Missing file**: all defaults (noted at `info`). Every key is optional, and a key whose value is `null` is the same as an absent key (also a whole section).
- **Malformed file → the engine refuses to start** with an error: text that is not a JSON object (or has anything but whitespace after it), nesting deeper than cJSON's limit (`third_party/cjson/README.md`), a file larger than `HAKKAS_CONFIG_FILE_MAX` (16 KiB, a per-build override), or a duplicate key at the top level or within a section.
- **Unknown keys** (also within a section) are logged as a warning and ignored, for forward compatibility.
- **Invalid values → the engine refuses to start**: every key with the wrong type or an out-of-range value is logged as an error naming it (`<file>: <key>: expected …`; the value itself is never logged), all of them, then startup stops. Valid values: `engine_id` a string of 1–64 bytes; `label` 0–64 bytes; `host` 1–63 bytes (not otherwise checked here; the listeners take only a numeric IPv4 or IPv6 address, `01-architecture-overview.md` §0.2, so a host name fails when they open in startup step 8 and the engine refuses to start); `port` an integer 1–65534 (`port + 1` must be a port too); `max_connections` an integer 1–1024; `max_message_bytes` an integer 1024–16777216; `allowed_origins` an array of at most 8 strings (`HAKKAS_CONFIG_ORIGINS_MAX`, a per-build override) of 1–127 bytes; `change_threshold` a number ≥ 0, `1e999` (+Infinity) included (`01-architecture-overview.md` §3.3); `log.level` one of the four names; `log.mirror`, `tls.enabled`, `net.dhcp` booleans; `auth.idle_timeout_s`, `auth.max_lifetime_s` integers 1–2147483647; `auth.server_secret` a string of 16–128 bytes; `net.hostname` 1–63 letters, digits and inner `-` (one DNS label); `net.address`/`netmask`/`gateway` a dotted-quad IPv4 address or `""` (none); a section a JSON object; paths below. An integer may be written in any JSON number form with an integral value (`8.0e3`).
- **Paths** (`webdir`, `datadir`, `auth.users_file`, `tls.cert`, `tls.key`): a relative path in the file is relative to the configuration directory (the file's own directory); a path given as a command-line option (§1.2) is used as given, i.e. relative to the working directory. A path (after that) is at most 251 bytes (`PAL_PATH_MAX` minus room for an atomic write's `.tmp`, §2.3). `webdir`, `datadir`, `auth.users_file` must not be empty; `tls.cert`/`tls.key` `""` means none (the default). The platform supplies the defaults that depend on it (configuration directory, `webdir`, `datadir`); the core derives the configuration file `<confdir>/hakkas.json` and the default users file `<confdir>/users.json` from the configuration directory (`/lfs/config` on Zephyr).
- **`engine_id`** unset: a fresh random UUID v4 (lowercase, the generator of `runtime:new_graph`'s ids, `src/system/uuid.h`) at every boot, after the command-line overrides.
- **`net.hostname`** `""` (the default) means the platform's default, `hakkas-<last 3 MAC bytes in hex>` on Zephyr.
- **`auth.server_secret`**: generated as 32 bytes from the PAL's random source, encoded in base64 (RFC 4648 §4: standard alphabet, `=` padding, 44 characters). The HMAC key of `02-wire-protocol.md` §2.3 is the string's bytes as stored, not decoded, so a hand-written secret is any string of 16–128 bytes. Only the startup of `run` (§1.4 step 1, plan task 19) generates and writes it; the other commands never write the file. It is written back by inserting the member as text — as the first member of the `auth` object, which is created if absent (a `null` `auth` or `server_secret` is replaced) — so every other byte of the user's file (formatting, key order, numbers, unknown keys) stays as it was. A missing file is created holding only the secret (mode `0600` on POSIX, like every file `pal_storage_write` creates). If the file cannot be written (e.g. the configuration directory is missing or read-only), the engine refuses to start. The secret is never logged (`02-wire-protocol.md` §2.5 item 8).
- **Base64**: a small encoder in the core (`src/system/base64.h`) until mbedTLS is integrated (plan task 15, `01-architecture-overview.md` §6), which may replace it with the same output.

### 1.2 POSIX build tree (Raspberry Pi, x86_64 development)

A command-line tree configures and starts the runtime:

- Global options: `--fine`/`--info`/`--warn` (log verbosity, most-to-least verbose; overrides `log.level`), `--basedir <dir>` (default `.`), `--confdir <dir>` (default `/etc/hakkas`).
- `list types` — print every registered block type name.
- `describe type <typename>` — print one block type's JSON descriptor: the `component:component` payload (`02-wire-protocol.md` §3.5).
- `describe topology <filename>` — load a graph file standalone and print its JSON description: the graph's canonical form (§2.1).
- `user add|passwd|del|list ...` — manage the credential store (`02-wire-protocol.md` §2.2).
- `run [options] process <filenames>` — start the engine and load the given colon-separated list of graph files, in order.
- `run [options] daemon` — start the engine and load every graph in `<datadir>/graphs/`.
- `run`'s own options override the corresponding configuration-file keys: `--id <id>` (`engine_id`), `--webdir <dir>`, `--host <addr>`, `--port <n>`, `--users <file>` (`auth.users_file`).

Implementation decisions (plan task 11, `src/platform/posix/cli.h`):
- **Grammar**: `hakkas [global options] <command>`. Global options come before the command and also include `--help`/`-h` (usage on stdout) and `--version`; `run`'s options come between `run` and `process`/`daemon`. An option's value is the next argument (no `--opt=value`), must not be empty, and a repeated option takes its last value. `user` accepts `add`, `passwd`, `del` and `list`; their own arguments are plan task 16's.
- **Configuration**: every command except `--help`/`--version` first builds the configuration — defaults < `<confdir>/hakkas.json` < options — so an unreadable, malformed or invalid file fails every command, not only `run`. `datadir` defaults to `--basedir`, `webdir` to `<working directory>/frontend/src`. `--fine`/`--info`/`--warn` already apply while the file is read; the resulting `log.level` is the log threshold.
- **Output**: results (type names one per line, a compact JSON document on one line) go to stdout; diagnostics go through the log (§4), whose POSIX sink is stderr.
- **Exit codes**: `0` success; `1` a runtime failure (configuration file, unknown type, a graph file that does not load, a command not implemented yet); `2` a usage error (unknown command or option, a missing or extra argument, an invalid option value such as `--port 0`).
- **Not implemented yet**: `user …` (plan task 16) and `run` (plan task 19; its options are parsed and applied already) are refused with an error and exit code `1`.

### 1.3 Zephyr build tree (MCU targets)

There is no argv to parse at boot. Configuration comes from Kconfig (compile-time limits and features), devicetree (board topology), and the configuration file on LittleFS (§1.1). The device always behaves like `run daemon`. An equivalent of the `list types`/`describe type`/`describe topology`/`user …` commands is exposed through Zephyr's shell subsystem over a console/USB-CDC backend, for interactive diagnostics and for provisioning users.

### 1.4 Startup sequence (both build trees)

1. Read the configuration file and apply command-line overrides (POSIX) → initialize logging and file locations. On Zephyr, first mount LittleFS.
2. Initialize the PAL: clock, random source, storage; on Zephyr also bring up networking (`04-drivers.md` §6). Delete any leftover `*.tmp` files from an interrupted write (§2.3). Create the engine loop (`loop_init`, which opens the PAL's cross-context handoff, `01-architecture-overview.md` §0.2) before step 5 starts any IO worker; on POSIX, install the `SIGINT`/`SIGTERM` stop handlers.
3. Load the credential store and initialize the authorization component (`02-wire-protocol.md` §2).
4. Register every built-in block type (`03-block-types.md`).
5. Initialize the IO provider: discover and configure hardware modules on Zephyr, or set up simulated channels on POSIX (`04-drivers.md` §0–§1).
6. Load composite block types from `<datadir>/types/` in dependency order: repeatedly register every type whose inner types are all registered, until no more can be registered. Anything left over (an unknown inner type, or a cycle) is logged as an error and skipped.
7. Load the configured graph files (POSIX `process`) or every graph in `<datadir>/graphs/` (POSIX `daemon`, Zephyr). Start each graph whose persisted `started` flag is `true` (§2.1).
8. Open the web server on `port` and the FBP WebSocket listener on `port + 1`, both on `host` (`loop_listen`); if either cannot be opened, the engine refuses to start.
9. Enter the engine loop (`01-architecture-overview.md` §3.2, §3.2.1). It runs until a stop request (POSIX: `SIGINT`/`SIGTERM`), then closes every connection and listener and returns.

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

Implementation decisions (plan task 10, `src/app/graph_files.h`):
- **Canonical form**: a save writes compact JSON (no whitespace) with the top-level keys in the order of the example and every key present (`description`/`icon` `""`, empty `blocks`/`connections`/`inports`/`outports`). Blocks are in creation order; each block's `metadata` has `x`, `y`, `change_threshold` (only when set), then its other keys in their order. Connections are listed per target block (creation order) and input (port order) — its initial or its edge — followed by the retained connections of placeholder blocks (§2.2) in load order. Graph ports: the live ones in creation order, then the retained ones.
- **Numbers are exact**: every number is written in `linkable_stringify`'s f64 form (the shortest text that reads back to the same double, `01-architecture-overview.md` §2.1, §6), opaque metadata included; an infinite `change_threshold` or opaque number is written `1e999`/`-1e999`.
- **Initial values** in `src.data`: a string and a boolean natively; a finite f64 as a JSON number; a non-finite f64 as the string `"NaN"`, `"Infinity"` or `"-Infinity"` (JSON has no such numbers), which a `"number"` port parses back to the same value (`02-wire-protocol.md` §3.4); a `Metric` as its string form `"<value> <unit>"`, which a `"number"` port parses back into a `Metric`; an i64 as its exact JSON integer text — it reads back as an f64, like every JSON number (`02-wire-protocol.md` §3.4; an i64 initial can only come from the C API, never from the wire or a file).
- **Round trip**: saving is a fixed point of loading. A file written by this code loads into a graph that saves to the same bytes; the first save of any other valid file is its canonical form — the same graph, in the order above, with numbers in their shortest exact form and initials in the type they were parsed into (`"5"` on a `"number"` port is written back as `5`, an integer beyond 2^53 as its nearest double).

### 2.2 Loading rules

- Each block's `component` is resolved against the block type registry. An unresolvable type name produces a **placeholder block**: it logs a warning, has no ports, and takes part in no dataflow. It keeps its original `component` name, metadata, and every connection that referenced it, and writes them back unchanged on save, so loading a graph on a system that lacks a type never destroys data. The one exception is an explicit user edit: a live edge or initial wired to an input that a retained connection feeds, or a live graph port taking a retained port's name, replaces the retained entry (`01-architecture-overview.md` §2.2), since the model never holds a fan-in.
- A connection that violates the graph's invariants (unknown node or port, fan-in) is logged and skipped, exactly as the equivalent wire command would be refused (`02-wire-protocol.md` §3.4.1).
- Initials are applied as their connections are loaded; the graph starts (if `started`) only after the whole file is loaded.

Implementation decisions (plan task 10, `src/app/graph_files.h`):
- **Reading**: a file is read through the PAL into one buffer sized from `pal_storage_stat`. A file larger than `HAKKAS_GRAPH_FILE_MAX` (default 256 KiB, a per-build compile-time override; the parsed tree takes several times the file's size) is refused before anything is read.
- **Malformed files are refused as a whole**, logged as an error, with nothing created. The minimum valid document is a JSON object (nothing but whitespace after it) whose `id` is a valid graph id (1–64 bytes, `02-wire-protocol.md` §3.4.1). `name`, `description` and `icon` must be strings, `started` a boolean, `blocks` an object, and `connections`, `inports`, `outports` arrays; each may be absent (`name` defaults as in `runtime:new_graph`, the others to `""`, `false` or empty). Text that is not JSON, nested deeper than cJSON's limit (`third_party/cjson/README.md`), or any other shape is malformed. A file whose `id` is already loaded is refused too (`Graph exists`).
- **Entries are loaded one by one** through the same registry commands as the wire (`runtime:new_graph`, `addnode`, `addedge`, `addinitial`, `addinport`/`addoutport`), so an entry is refused exactly as the command would be, logged as a warning with the command's message (`02-wire-protocol.md` §3.4.1), and skipped; the rest still loads. Besides those refusals an entry is skipped when it has the wrong shape: a block that is not an object with a string `component`, or whose `metadata` has a non-numeric (or non-finite) `x`/`y` or an invalid `change_threshold`; a connection whose `src`/`tgt` are not `{node, port}` strings or `{data}` with a string, number or boolean, or whose edge `metadata` is not an object; a graph port that is not `{name, node, port}` strings. Fan-in: the first connection to an input wins, live or retained. Only running out of memory aborts a load, and then the partly loaded graph is deleted.
- **Placeholders**: an entry whose block is a placeholder is kept as a retained entry (`01-architecture-overview.md` §2.2) when both its blocks exist; the end that is a real block is still checked like the wire command (an unknown port, a wrong direction or a fan-in skips it). Retained entries are not visible on the wire; the placeholder block itself is (`02-wire-protocol.md` §3.4).
- **Events**: loading reports the events those commands report (graph created, each block, edge, initial and graph port in file order, then `network:started` if `started`). Startup loads graphs before the listeners open (§1.4), so no client receives them; a later load (none exists yet) would broadcast them like the commands.

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

**Implementation decisions** (plan task 10, `src/app/file_names.h`, `src/app/graph_files.h`): decoding a file name accepts only what encoding produces (no `.json` suffix, a kept byte written as `%XX`, lowercase hex, a raw byte that needs escaping, or more than 64 decoded bytes are not graph files), so a decoded name always encodes back to the same file name. Paths are `<datadir>/graphs/<encoded id>.json` (a trailing `/` of `datadir` is ignored); a path that would not fit `PAL_PATH_MAX` with its `.tmp` suffix is refused. `<datadir>/graphs/` and `<datadir>/types/` are created if missing (one level, `datadir` itself must exist), and saving creates `graphs/` on demand. `run daemon` loads every regular file ending in `.json` in `<datadir>/graphs/` (so no `.tmp` leftover), in byte order of the file names, so graphs are created in the same order every boot; a file whose name is not the encoding of its graph's `id` is loaded with a warning (`network:persist` writes to the id's file). `run process` loads the given colon-separated files in order. Every file is tried; a failed one is logged and skipped.

**Deleting**: `runtime:delete_graph` removes the graph's file (a graph that was never persisted has none; any other failure to remove it is logged as an error, and the deletion stands — the file would load again at the next boot); removing a group removes its type file (`02-wire-protocol.md` §3.2, §3.4.2). Renaming a graph changes only its `name`, never its `id`, so the file name stays the same.

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
