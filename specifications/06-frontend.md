# 06 — Frontend

The frontend is the browser editor in which users see and change the running processes. It lives in the repository, in `frontend/`, and is built and versioned together with the engine. It speaks the wire protocol of `02-wire-protocol.md`, is served by the engine's web server (`05-app-lifecycle-persistence.md` §3) and holds no state of its own beyond what the engine's events tell it.

**Status**: this is the second generation of the UI. The first was built on GoJS (commercial); this one is a restart on the open-source **Rete** (v2) editor framework and is far from complete. Features are added step by step, because several design decisions depend on how incomplete versions are actually used. §7 lists what is missing.

**Technology**: Vue 3 with TypeScript, built by Vite; Rete v2 with its area, connection, Vue rendering, context menu, minimap, reroute and auto-arrange plugins; `@noble/hashes` for the login's cryptography (§2). The styles are the frontend's own (§4.1; Bootstrap was dropped with the themes, 2026-10-14). Dependencies are pinned in `frontend/package-lock.json`.

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
- **Resuming after a reload** (`02-wire-protocol.md` §2.3.1, implemented in plan task 46): every login reply's `resume` id is kept in the browser's `localStorage` (key `hakkas.session`) with the user name and the resume key — `HMAC(StoredKey, "Resume Key," + nonce)`, computed with the login's proof and never sent; the password is not kept. When the page loads and a session is kept, the frontend resumes it at once (the login form shows "Resuming the session..." meanwhile) and keeps the reply's new id: an id is used once. A resume that fails (the session has expired, the engine restarted, another tab used the id) forgets the kept session and shows the login form **without** an error message: that is the normal end of a session, not a mistake. A logout and a session expiry forget it too. A connection that cannot be made keeps it (the message says the server cannot be reached; the next reload tries again). The kept session is per browser and origin, so tabs share it: a reload of any tab resumes the chain and moves its id on.
- **Login** (`02-wire-protocol.md` §2.3): `environment:challenge` with the user name and a client nonce of 18 random bytes, then `environment:login` with the SCRAM-SHA-256 proof. The key stretching (PBKDF2, 100 000 iterations by default) takes a few hundred milliseconds; the form shows that it is logging in meanwhile. The password is kept only until the proof is computed. The client checks the reply's `server_signature` and refuses a login whose signature does not match ("The server could not prove its identity"), so a device that does not know the user's server key is not trusted. A `network:error` before the login has succeeded (e.g. `"Login failed"`) is shown on the login screen.
- **Cryptography in JavaScript**: `crypto.subtle` exists only in secure contexts, and the device is normally reached as plain `http://<device-ip>` (`02-wire-protocol.md` §2.3, "Browser note"), so the frontend always uses `@noble/hashes` (PBKDF2-HMAC-SHA256, HMAC, SHA-256), audited pure JavaScript, one code path everywhere.
- **Nothing secret in the console**: the frontend logs each message's protocol and command, and its payload except for the `environment` protocol, whose payloads carry the session secret, nonces, proofs and resume ids. An outgoing message's `secret` is never logged.
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
- A block added from the palette is named after its type with the first number not taken in the process: `math/Add2` → `Add2_1`, `Add2_2`, … The name goes in the `graph:addnode` command; the block appears with its event.
- When the snapshot has arrived, the view zooms to fit the process. The snapshot has no end marker, so this happens after a pause of 150 ms in the events following a `graph:clear`. The editor's element carries `data-settled="true"` once that fit is done (`false` from the `graph:clear` until then): before it, blocks move under the pointer, so the tests (§6) wait for it before they touch the view.

### 4.0 Selecting blocks

Implemented 2026-10-14 (plan task 26). The **selection** is the view's own state, not the engine's: it is not sent, and another client does not see it. What is done *to* the selection goes to the engine as commands, and the view changes only with their events (`02-wire-protocol.md` §1.1).

- **Selecting**: a click on a block selects it alone; Ctrl-click or Shift-click (⌘ on a Mac) adds an unselected block or unselects a selected one. Pressing a selected block keeps the selection, so that dragging it drags all of them; a plain click on it (no movement) then selects it alone.
- **The marquee**: a Shift- or Ctrl-drag on the empty canvas draws a rectangle; the blocks whose middle is inside it are added to the selection. A plain drag on the canvas still pans the view.
- **Clearing**: Escape, or a click on the empty canvas (a press released within a few pointer movements; a drag pans and keeps the selection). **Ctrl+A** (⌘A) selects every block of the process.
- **Delete or Backspace** sends one `graph:removenode` per selected block (the engine removes their connections too); each block leaves the view, and the selection, with its event. With nothing selected the keys do nothing. Keys typed into a field are the field's.
- **Moving the selection**: dragging a selected block moves every selected block by the same amount. No block moves locally; each one's new position goes to the engine as `graph:changenode`, at most one command per block and animation frame (the latest position), and the final positions when the pointer is released. Rete's own node selection (`selectableNodes`) is not used, since it moves the other selected blocks locally.
- A block removed by an event leaves the selection, and opening a process starts with none; a renamed block stays selected.

### 4.2 The inspector

Implemented 2026-10-14 (plan task 27). A panel to the right of the editor shows the selection (§4.0); it is read-only for now, the base for renaming blocks, fixed values and live values (plan tasks 28, 29, 34).

- **Nothing selected**: a hint to select a block.
- **One block**: its id; its type; the type's description; its inputs, then its outputs, in the type's port order, each with its id, type, description and default when the type gives one. All of it comes from the type's `component:component`, so it is the engine's own description. A block whose type is not registered (a placeholder, `05-app-lifecycle-persistence.md` §2.2) is said to be one, without ports.
- **Several blocks**: their number and ids.
- **Actions**: Delete, the same as the Delete key (§4.0), for one or several blocks.
- It follows the events: a block renamed by another client shows its new id, and a removed block leaves it.

### 4.1 Themes

Decided 2026-10-14: the frontend's look comes from **pre-installed themes**, plain CSS files chosen by the viewer; there is no theme editor and no per-user setting in the engine.

- **What a theme is**: one stylesheet, `public/themes/<id>.css` (served as `/themes/<id>.css`, copied into the build as it is, not bundled), that sets **only CSS custom properties** — the `--hk-*` variables: page, surfaces, text, borders and their width, the accent and focus colors, errors, the running/stopped marks, and the editor's canvas, blocks, block titles, sockets, connections and minimap. `src/assets/hakkas.css`, bundled, styles every part of the UI from those variables and sets no color itself; it also restyles Rete's own parts (blocks, sockets, connections, minimap, context menu), whose built-in colors it overrides under the editor's id. A unit test checks that every installed theme sets exactly the variables `hakkas.css` uses.
- **The installed themes** are listed by `/themes/themes.json`, `[{"id", "name"}]` in the selector's order. Three are delivered: **Light**, **Dark** and **High contrast** (white and yellow on black, two-pixel borders, cyan connections). Since a theme is a file in `webdir`, another can be installed by adding its stylesheet and its entry in `themes.json`, without building the frontend; an id is 1–32 characters of `a–z`, `0–9` and `-`.
- **Choosing**: a selector next to Logout (shown when more than one theme is installed) switches at once. The choice is the **viewer's, per browser** (`localStorage`, key `hakkas.theme`): it is a viewing preference, not engine state, so it is not sent to the engine (02 §1.1 concerns the model, not the look). Without a choice — or when the stored theme is no longer installed, or storage is unavailable — the theme follows the system: High contrast when it asks for more contrast (`prefers-contrast: more`), else Dark or Light (`prefers-color-scheme`).
- **No flash**: `index.html` applies the theme with a few lines of inline script before the first paint, by the same rules as `src/theme.ts`; at startup `main.ts` then reads `themes.json` and replaces a stored theme that is no longer installed by the default, already on the login screen.

## 5. On the device

On Colibri (`04-drivers.md` §1) the frontend is stored on external flash: the MCU board carries a 16 MB W25Q128, mounted with LittleFS at `/mcu`, and the carrier board another, mounted at `/carrier` (`05-app-lifecycle-persistence.md` §1.3). The assets are plain files there, so the frontend can be updated without reflashing the firmware.

**Size**: the current bundle is about 4.0 MB, 2.5 MB compressed with gzip — mostly a JavaScript file of 2.1 MB (0.63 MB compressed) and Material Icons fonts in every style, 1.8 MB, which are already compressed; the styles are 12 KB and each theme about 1.5 KB (Bootstrap's 0.24 MB went with the themes, §4.1). That fits the flash comfortably, but it is written over SPI and every update rewrites it, so the bundle should shrink before the device ships: the 3D editor template pulls in three.js and is not used; the icon fonts are not used by any part of the UI yet; and the build can store each asset as `<file>.gz` only, which the web server serves to every browser (`05-app-lifecycle-persistence.md` §3).

## 6. Testing

Two suites, both in `frontend/tests/` (decided 2026-10-13):

- **Unit tests** (`tests/unit/`, Vitest, `npm test`, in Node without a browser or an engine): the protocol and model code. Messages enter through a real `Connection`'s dispatch (`onMessage`) with the socket left closed, and what it sends is recorded (`tests/unit/harness.ts`). They cover the themes (§4.1), the SCRAM computation, the resume key and proof, the login and resume handshakes (against a server side written with `node:crypto`, `tests/support/scram-server.ts`, independent of the frontend's `@noble/hashes` code), the kept session (§2), the WebSocket URL (§2), the client model fed by events (§3), new blocks' names (§4), the selection, a dragged selection's positions and their throttling (§4.0), and what the inspector shows (§4.2).
- **End-to-end tests** (`tests/e2e/`, Playwright, `npm run test:e2e`, which builds `frontend/dist` first): the built frontend in a browser against the real engine. **Each test starts its own engine** (`tests/e2e/fixtures.ts`): the `hakkas` executable (`HAKKAS_BIN`, default `build/debug/hakkas`, so the C build comes first) on free loopback ports, with a temporary configuration directory (a `hakkas.json` a test may extend, e.g. a short `auth.idle_timeout_s`), a users file with an editor and a viewer whose records the fixture computes with few PBKDF2 iterations, and the test's processes in `<datadir>/graphs/` (the sister project's `examples/example.json` by default). A test may also use a **second client** (`tests/e2e/protocol-client.ts`), a logged-in WebSocket client in Node, to act as another user or to observe what the frontend sent. Every test fails if the page reports an error or a graph event fails to apply, if the engine logs an error, or if it does not stop cleanly (exit code 0 after `SIGINT`); the engine's log is attached to the test's report. The suites cover the themes (switching, the choice kept across a reload, the default from the system, a theme no longer installed), login (success, refusals, retry, logout, session expiry, a lost connection, nothing secret in the console), resuming (reloads stay logged in, a logout or a session that cannot be resumed asks for the password without an error), rendering (§4, against the engine's own snapshot), and the command → event rule in both directions: another client's edits appear; the palette, dragging and drawing a connection reach the engine and come back as its events; a refused command changes nothing and shows the engine's error; and selecting (clicks, the marquee, Escape, Ctrl+A, the canvas), dragging a selection and deleting it (§4.0); and the inspector against the engine's description of a type, with several blocks, and following another client's rename and removal (§4.2).
- **Browser**: Playwright's Chromium (`npx playwright install chromium`, once per machine), or any Chromium-based browser named by `HAKKAS_BROWSER` (e.g. an installed Chrome or Brave). Reports and failure artifacts (screenshots, traces) go to `frontend/test-results/`, which is not committed.
- The suites are not part of CTest: the C build needs no Node.js toolchain (§1).

## 7. Not implemented yet

The order of the work is phase 5 of `docs/plan.md`. Decided with the user for it (2026-10-14):

- **Saving**: a Save button and an "unsaved" mark per process, from `network:status`'s `unsaved` (`02-wire-protocol.md` §3.3, a protocol addition). A successful save is confirmed discreetly (a short notice that fades); a failed one is shown prominently (a dialog, or an error that stays until dismissed).
- **Starting at boot** is the saved `started` state: the process list shows the loaded processes only (as now), each with start/stop; a start or stop makes the process unsaved, and saving it stores whether it starts at boot (`05-app-lifecycle-persistence.md` §2.1; `run daemon` starts the processes saved running; with `run processes` a list entry's own `start` wins, 05 §1.2).
- **A process's description** stays read-only in the UI for now (set at creation; no command changes it).
- **Composite types** are view-only for now: create, describe, rename and remove them, and look inside one; editing an existing type's inside is wanted in the long term and needs its own design (what happens to running instances).
- **Fewer logins**: reloading the page does not ask for the password again — the session is resumed with a resume key (`02-wire-protocol.md` §2.3.1, a protocol addition), kept in the browser's `localStorage` until the session would have expired or the user logs out. Implemented (§2, plan task 46).
- **Block categories**: the bitwise `math/And2` … blocks are gone and the comparisons are `compare/*` (`03-block-types.md` §3, plan task 47); the palette's sections are the type names' prefixes, so `compare` is a section of its own.

What the frontend does today is §2–§4: log in (and resume after a reload), list the block types and processes, open a process and draw it, and follow every change the engine broadcasts. Selecting blocks, moving and deleting the selection are §4.0, the inspector §4.2. The other editing commands are sent from the editor's hooks — `addnode` (from the block palette), `removenode` (the context menu), `addedge` and `removeedge` — and `runtime:new_graph` from the process list, but they have only been tried in passing. Missing, roughly in the order they are likely to be needed:

- **Live values**: `network:edges` subscriptions and `network:data` shown on connections or ports (`02-wire-protocol.md` §4.2).
- Starting, stopping and saving a process (`network:start`/`stop`/`persist`, with the unsaved mark above), deleting and renaming it.
- Editing initials, renaming blocks.
- The role's capabilities (`02-wire-protocol.md` §2.4): a viewer or operator should get a read-only editor.
- Graph inports/outports (`graph:addinport` …), groups (`02-wire-protocol.md` §3.4.2), links between processes (`00-README.md` open question 13) and the environment's ports (`03-block-types.md` §4), none of which is drawn yet.
- Reconnecting automatically after a lost connection, resuming the session (§2) instead of asking for the password.
- A smaller bundle (§5).
