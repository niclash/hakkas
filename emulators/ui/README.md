# Emulator UI

The generic browser UI of hakkas' emulators (`../specifications/00-README.md`): it knows nothing
about any device and draws what an emulator describes over its control protocol
(`../specifications/01-control-protocol.md`). Vue 3 and Vite, like hakkas' frontend. An emulator
serves the built files (`dist/`) on its HTTP port, so the page and the control WebSocket share
one origin.

> Work in progress: the signal panel, the environment library, the settings and the counters
> (Colibri emulator plan tasks E12–E13). The board with its faceplates (E14) follows.

## Building and testing

```sh
npm install
npm run build        # dist/, which the emulator serves
npm test             # unit tests (vitest)
npm run test:e2e     # builds, then the browser tests against a real Colibri emulator
```

The browser tests start `../colibri/dist/main.js` (or `COLIBRI_EMULATOR`) once per test, so
build the emulator first (`npm install && npm run build` in `../colibri`). They use
Playwright's Chromium (`npx playwright install chromium` once) or a Chromium-based browser
named by `EMULATOR_BROWSER`.

## Developing

```sh
colibri-emulator …            # an emulator on its default HTTP port, 8090
npm run dev                   # the UI with hot reloading; /control and /assets go to the emulator
```

`EMULATOR_HTTP` names another emulator (default `http://localhost:8090`).
