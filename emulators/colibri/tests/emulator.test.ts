import {mkdtempSync, rmSync, writeFileSync} from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {afterEach, beforeEach, describe, expect, it} from 'vitest';
import {loadCatalog} from '../src/catalog.js';
import {openEnvironment} from '../src/emulator.js';
import {Library} from '../src/library.js';
import {createLogger} from '../src/log.js';
import {BUILTIN_DATA} from '../src/package.js';

const catalog = await loadCatalog([BUILTIN_DATA], createLogger('error', () => {}));
let tmp: string;
let lib: Library;
let lines: string[];
const log = createLogger('fine', l => lines.push(l));

beforeEach(async () => {
    tmp = mkdtempSync(path.join(os.tmpdir(), 'colibri-open-'));
    lib = new Library(tmp);
    await lib.init();
    lines = [];
});

afterEach(() => rmSync(tmp, {recursive: true, force: true}));

describe('opening the environment to run', () => {
    it('logs it and remembers it ran', async () => {
        writeFileSync(lib.file('lab'), JSON.stringify({carrier: 'colibri_8', cycleMs: 20,
            modules: [{slot: 1, type: 'colibri_aiv'}, {slot: 2, type: 'colibri_flux', colour: 'red'}]}));
        expect(await openEnvironment(lib, 'lab', catalog, log)).toBeDefined();
        expect(lines).toEqual([
            'warn environment lab: modules[1].colour: unknown member, ignored',
            'warn environment lab: slot 2: unknown module type colibri_flux, the slot stays empty',
            'info environment lab: carrier colibri_8, modules 2, cycle 20 ms',
        ]);
        expect(await lib.lastRun()).toBe('lab');
    });

    it('logs why one cannot be run, and does not remember it', async () => {
        writeFileSync(lib.file('bad'), JSON.stringify({carrier: 'colibri_8', modules: [{slot: 9, type: 'colibri_aiv'}]}));
        writeFileSync(lib.file('broken'), '[');
        expect(await openEnvironment(lib, 'bad', catalog, log)).toBeUndefined();
        expect(await openEnvironment(lib, 'broken', catalog, log)).toBeUndefined();
        expect(lines).toEqual([
            'error environment bad: slot 9: the carrier has slots 0 to 7',
            'error environment bad cannot be run',
            expect.stringMatching(/^error environment broken: .*JSON/),
            'error environment broken cannot be run',
        ]);
        expect(await lib.lastRun()).toBeUndefined();
    });
});
