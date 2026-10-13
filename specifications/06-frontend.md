# 06 — Frontend

The frontend is the browser editor in which users see and change the running processes. It lives in the repository, in `frontend/`, and is built and versioned together with the engine. It speaks the wire protocol of `02-wire-protocol.md`, is served by the engine's web server (`05-app-lifecycle-persistence.md` §3) and holds no state of its own beyond what the engine's events tell it.

**Status**: this is the second generation of the UI. The first was built on GoJS (commercial); this one is a restart on the open-source **Rete** (v2) editor framework and is far from complete. Features are added step by step, because several design decisions depend on how incomplete versions are actually used. §6 lists what is missing.

**Technology**: Vue 3 with TypeScript, built by Vite; Rete v2 with its area, connection, Vue rendering, context menu, minimap, reroute and auto-arrange plugins; Bootstrap for layout; `@noble/hashes` for the login's cryptography (§2). Dependencies are pinned in `frontend/package-lock.json`.

## 1. Building and serving

`npm install && npm run build` in `frontend/` type-checks the sources (`vue-tsc`) and builds the bundle into `frontend/dist` (`index.html` and `assets/`). Neither `node_modules/` nor `dist/` is committed. The engine's CMake build does not build the frontend, so working on the C code needs no Node.js toolchain.

The web server serves `webdir` (`05-app-lifecycle-persistence.md` §1.1). Its default is chosen so that neither case needs an option (decided 2026-10-13):

- **Developing hakkas** (POSIX): the default is the source tree's `frontend/dist`, an absolute path the CMake build compiles in (`HAKKAS_DEFAULT_WEBDIR`). The engine finds the frontend from any working directory: a shell in the repository, CLion's run configurations, a scratch directory for test data.
- **Packaged for users** (POSIX): configuring with `-DHAKKAS_PACKAGE=ON` makes the default `<prefix>/share/hakkas/web` (`CMAKE_INSTALL_FULL_DATADIR`), and `cmake --install` installs the executable and copies `frontend/dist` there (the install fails if the frontend has not been built). A package build never refers to the source tree.
- **Zephyr**: the assets are files on the device's flash (§5), under the default `webdir` of `05-app-lifecycle-persistence.md` §1.3.
- In every case `webdir` in `hakkas.json` and `--webdir` override the default. At startup the web server warns when `webdir` has no `index.html` (nor `index.html.gz`) — usually a frontend that has not been built — instead of answering every request with `404` in silence.

**Development server**: `npm run dev` serves the frontend from Vite, with live reloading, on its own port (5173). The page's port + 1 is then not the engine's WebSocket, so the environment variable `VITE_HAKKAS_WS` gives its URL (e.g. `ws://localhost:8081/`), and the engine must accept the page's origin: `"allowed_origins": ["http://localhost:5173"]` in `hakkas.json` (`02-wire-protocol.md` §0). `npm run build` is run without `VITE_HAKKAS_WS`, so the bundle uses the page's port + 1.

## 2. Connection and login

- **WebSocket URL**: `ws://<page host>:<page port + 1>/` (`wss://` for an `https://` page), as `02-wire-protocol.md` §0 requires; the port of a page without one is the scheme's default (80, 443). An IPv6 host is written in brackets.
- **One connection per login**: logging in opens a new WebSocket; logging out, a session expiry (`environment:logout`), a failed login and a lost connection close it and return to the login screen. A lost connection says so on that screen; there is no automatic reconnection yet.
- **Login** (`02-wire-protocol.md` §2.3): `environment:challenge` with the user name and a client nonce of 18 random bytes, then `environment:login` with the SCRAM-SHA-256 proof. The key stretching (PBKDF2, 100 000 iterations by default) takes a few hundred milliseconds; the form shows that it is logging in meanwhile. The password is kept only until the proof is computed. The client checks the reply's `server_signature` and refuses a login whose signature does not match ("The server could not prove its identity"), so a device that does not know the user's server key is not trusted. A `network:error` before the login has succeeded (e.g. `"Login failed"`) is shown on the login screen.
- **Cryptography in JavaScript**: `crypto.subtle` exists only in secure contexts, and the device is normally reached as plain `http://<device-ip>` (`02-wire-protocol.md` §2.3, "Browser note"), so the frontend always uses `@noble/hashes` (PBKDF2-HMAC-SHA256, HMAC, SHA-256), audited pure JavaScript, one code path everywhere.
- **Nothing secret in the console**: the frontend logs each message's protocol and command, and its payload except for the `environment` protocol, whose payloads carry the session secret, nonces and proofs. An outgoing message's `secret` is never logged.
- After the login the frontend sends `runtime:getruntime`, which brings the block types (`component:component`) and the processes (`network:status`).

## 3. The view follows the engine's events

The frontend obeys `02-wire-protocol.md` §1.1 strictly: **a user action only sends a command; the view changes only when the resulting event arrives**, for the sender as for every other client. A refused command therefore needs no rollback (its `network:error` is shown in a dismissable list, the latest five), and several users editing one process see the same state.

- **The client model** (`frontend/src/components/model.ts`) is fed in one place, attached to each connection before its login: the block types from `component:component` (replacing a type of the same name, so a changed group type updates in place); the processes from `network:status`, `runtime:new_graph`, `runtime:delete_graph`, `network:started`/`stopped` and `graph:rename`; the open process from `graph:clear`. The Vue components only render it.
- **The open process** is selected by `graph:connect`. Its view is cleared by the `graph:clear` that starts the snapshot (`02-wire-protocol.md` §4.1.1) — not when the user clicks — and built from the snapshot and the events after it.
- **Events are applied one at a time, in arrival order**: applying an event to the Rete editor is asynchronous, so each graph event waits for the previous one to finish (a promise chain). Otherwise an `addedge` could be applied before the `addnode`s of its blocks have completed and be lost.
- **Events of other processes** are ignored by the view. Before its first `graph:connect` a connection receives the graph events of every process (`02-wire-protocol.md` §4.1); only the open process's are drawn.
- While an event is being applied the editor's own change hooks are disabled, so drawing an event never sends a command back.

## 4. Rendering a process

- A **block** is a Rete node titled with the block id, its ports taken from its type's `component:component`: outputs first, then inputs, each in the type's port order. A block whose type is not registered (a placeholder, `05-app-lifecycle-persistence.md` §2.2) is drawn without ports.
- A block's **position** is its metadata's `x`/`y`, from `graph:addnode` and `graph:changenode`. Other metadata is not used yet.
- A **connection** is a Rete connection between the two ports.
- An **initial** is shown in its input's label, `in1 = 12`; removing it restores the plain port name. The value is shown as the event carries it (a string, `02-wire-protocol.md` §3.4).
- When the snapshot has arrived, the view zooms to fit the process. The snapshot has no end marker, so this happens after a pause of 150 ms in the events following a `graph:clear`.

## 5. On the device

On Colibri (`04-drivers.md` §1) the frontend is stored on external flash: the MCU board carries a 16 MB W25Q128, mounted with LittleFS at `/mcu`, and the carrier board another, mounted at `/carrier` (`05-app-lifecycle-persistence.md` §1.3). The assets are plain files there, so the frontend can be updated without reflashing the firmware.

**Size**: the current bundle is about 4.3 MB, 2.5 MB compressed with gzip — mostly a JavaScript file of 2.2 MB (0.65 MB compressed) and Material Icons fonts in every style, 1.8 MB, which are already compressed. That fits the flash comfortably, but it is written over SPI and every update rewrites it, so the bundle should shrink before the device ships: the 3D editor template pulls in three.js and is not used; only one icon font style is needed, or none; and the build can store each asset as `<file>.gz` only, which the web server serves to every browser (`05-app-lifecycle-persistence.md` §3).

## 6. Not implemented yet

What the frontend does today is §2–§4: log in, list the block types and processes, open a process and draw it, and follow every change the engine broadcasts. The editing commands are sent from the editor's hooks — `addnode` (from the block palette), `removenode`, `addedge`, `removeedge`, and `changenode` while a block is dragged — and `runtime:new_graph` from the process list, but they have only been tried in passing. Missing, roughly in the order they are likely to be needed:

- **Dragging** sends one `changenode` per pointer movement, and the block moves when each event returns. That is smooth on a local network; over the Internet it will need throttling (e.g. at most one command per animation frame, and the final position on release), still without moving the block locally.
- **Live values**: `network:edges` subscriptions and `network:data` shown on connections or ports (`02-wire-protocol.md` §4.2).
- Starting, stopping and persisting a process (`network:start`/`stop`/`persist`), deleting and renaming it.
- Editing initials, renaming blocks.
- The role's capabilities (`02-wire-protocol.md` §2.4): a viewer or operator should get a read-only editor.
- Graph inports/outports (`graph:addinport` …), groups (`02-wire-protocol.md` §3.4.2), links between processes (`00-README.md` open question 13) and the environment's ports (`03-block-types.md` §4), none of which is drawn yet.
- Reconnecting automatically after a lost connection.
- A smaller bundle (§5).
