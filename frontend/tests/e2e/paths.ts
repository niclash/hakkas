import {fileURLToPath} from "node:url";
import {resolve} from "node:path";

const frontend = fileURLToPath(new URL("../..", import.meta.url));

// The engine under test: HAKKAS_BIN, else the debug preset's build.
export const HAKKAS_BIN = process.env.HAKKAS_BIN ?? resolve(frontend, "../build/debug/hakkas");
// The frontend under test, built by `npm run test:e2e` (vite build) before the tests; or
// HAKKAS_DIST, e.g. frontend/dist-device (`npm run build:device`: every file gzip only, 06 §5).
export const DIST_DIR = process.env.HAKKAS_DIST ?? resolve(frontend, "dist");
export const EXAMPLE_GRAPH = resolve(frontend, "../examples/example.json");
export const LIBRARY_DIR = resolve(frontend, "../library");
