import {fileURLToPath} from 'node:url';
import {resolve} from 'node:path';

const ui = fileURLToPath(new URL('../..', import.meta.url));

// The emulator under test: COLIBRI_EMULATOR, else ../colibri's build.
export const EMULATOR_MAIN = process.env.COLIBRI_EMULATOR ?? resolve(ui, '../colibri/dist/main.js');
// The UI under test; the emulator serves it from here (its UI_DIST).
export const UI_DIST = resolve(ui, 'dist');
