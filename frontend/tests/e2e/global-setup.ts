import {existsSync} from "node:fs";
import {DIST_DIR, HAKKAS_BIN} from "./paths";

// Fails early, with the remedy, when the engine or the frontend's build is missing.
export default function globalSetup() {
    if (!existsSync(HAKKAS_BIN))
        throw new Error(`no hakkas executable at ${HAKKAS_BIN}: build it (cmake --preset debug && ` +
                        `cmake --build --preset debug) or set HAKKAS_BIN`);
    if (!existsSync(DIST_DIR + "/index.html") && !existsSync(DIST_DIR + "/index.html.gz"))
        throw new Error(`no frontend build in ${DIST_DIR}: run the tests with npm run test:e2e, which builds it`);
}
