// The package's own files, found relative to this module, so the same paths hold in `src/` (tests)
// and in `dist/` (the built emulator).
import {readFileSync} from 'node:fs';
import {fileURLToPath} from 'node:url';

export const PACKAGE_ROOT = fileURLToPath(new URL('..', import.meta.url));

// The built-in plugin directory (spec §7): modules, carriers and communication modules.
export const BUILTIN_DATA = fileURLToPath(new URL('../data', import.meta.url));

export const VERSION: string =
    JSON.parse(readFileSync(new URL('../package.json', import.meta.url), 'utf8')).version;
