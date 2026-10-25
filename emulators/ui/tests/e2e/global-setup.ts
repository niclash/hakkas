import {existsSync} from 'node:fs';
import {EMULATOR_MAIN, UI_DIST} from './paths';

// Fails early, with the remedy, when the emulator or the UI's build is missing.
export default function globalSetup() {
    if (!existsSync(EMULATOR_MAIN))
        throw new Error(`no emulator at ${EMULATOR_MAIN}: build it (npm install && npm run build in ` +
                        `emulators/colibri) or set COLIBRI_EMULATOR`);
    if (!existsSync(`${UI_DIST}/index.html`))
        throw new Error(`no UI build in ${UI_DIST}: run the tests with npm run test:e2e, which builds it`);
}
