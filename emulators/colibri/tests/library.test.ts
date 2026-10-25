import {existsSync, mkdirSync, mkdtempSync, readdirSync, readFileSync, rmSync, writeFileSync} from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {afterEach, beforeEach, describe, expect, it} from 'vitest';
import {newEnvironment} from '../src/environment.js';
import {Library, LibraryError} from '../src/library.js';

let tmp: string;
let lib: Library;

beforeEach(async () => {
    tmp = mkdtempSync(path.join(os.tmpdir(), 'colibri-library-'));
    lib = new Library(path.join(tmp, 'lib'));
    await lib.init();
});

afterEach(() => rmSync(tmp, {recursive: true, force: true}));

function put(name: string, content: unknown): void {
    writeFileSync(path.join(lib.environments, name), typeof content === 'string' ? content : JSON.stringify(content));
}

function files(): string[] {
    return readdirSync(lib.environments).sort();
}

async function refused(p: Promise<unknown>, message: string): Promise<void> {
    await expect(p).rejects.toThrow(LibraryError);
    await expect(p).rejects.toThrow(message);
}

describe('the library', () => {
    it('makes its directories', () => {
        expect(existsSync(lib.environments)).toBe(true);
        expect(existsSync(lib.bindings)).toBe(true);
    });

    it('lists the environments by name', async () => {
        put('b.json', {});
        put('a.json', {});
        put('.hidden.json', {});
        put('notes.txt', 'x');
        mkdirSync(path.join(lib.environments, 'dir.json'));
        expect(await lib.list()).toEqual(['a', 'b']);
    });

    it('creates an environment with the defaults', async () => {
        await lib.create('lab');
        expect(await lib.read('lab')).toEqual({env: newEnvironment(), errors: [], warnings: []});
        await refused(lib.create('lab'), 'Environment exists: lab');
        expect(files()).toEqual(['lab.json']);
    });

    it('duplicates an environment’s file as it is', async () => {
        put('lab.json', '{"carrier": "colibri_8",   "note": 1}');
        await lib.create('copy', 'lab');
        expect(readFileSync(lib.file('copy'), 'utf8')).toBe('{"carrier": "colibri_8",   "note": 1}');
        await refused(lib.create('other', 'nope'), 'Unknown environment: nope');
        await refused(lib.create('lab', 'copy'), 'Environment exists: lab');
        expect(files()).toEqual(['copy.json', 'lab.json']);
    });

    it('refuses names that are no environment’s', async () => {
        for (const name of ['', '.x', 'a/b', '../up'])
            await refused(lib.create(name), 'Invalid name');
        await refused(lib.read('a b'), 'Invalid name');
        await refused(lib.rename('a', '.b'), 'Invalid name');
    });

    it('reads what is wrong with a file', async () => {
        put('broken.json', '{"carrier": ');
        put('bad.json', {carrier: 'colibri_8', cycleMs: -1});
        expect((await lib.read('broken')).env).toBeUndefined();
        expect((await lib.read('broken')).errors[0]).toMatch(/JSON/);
        expect(await lib.read('bad')).toEqual({errors: ['cycleMs: expected an integer from 1 to 1000'], warnings: []});
        await refused(lib.read('nope'), 'Unknown environment: nope');
    });

    it('writes atomically, leaving no temporary file', async () => {
        await lib.create('lab');
        const env = {...newEnvironment(), cycleMs: 20};
        await lib.write('lab', env);
        expect((await lib.read('lab')).env).toEqual(env);
        expect(files()).toEqual(['lab.json']);
    });

    it('renames without replacing', async () => {
        await lib.create('a');
        await lib.create('b');
        await refused(lib.rename('a', 'b'), 'Environment exists: b');
        await refused(lib.rename('nope', 'c'), 'Unknown environment: nope');
        await lib.rename('a', 'c');
        expect(files()).toEqual(['b.json', 'c.json']);
        await lib.rename('c', 'c');
        expect(files()).toEqual(['b.json', 'c.json']);
    });

    it('deletes', async () => {
        await lib.create('a');
        await lib.delete('a');
        expect(files()).toEqual([]);
        await refused(lib.delete('a'), 'Unknown environment: a');
    });

    it('remembers which environment ran last, also across a rename', async () => {
        expect(await lib.lastRun()).toBeUndefined();
        await lib.create('a');
        await lib.setLastRun('a');
        expect(await lib.lastRun()).toBe('a');
        await lib.rename('a', 'b');
        expect(await lib.lastRun()).toBe('b');
        writeFileSync(path.join(lib.dir, 'state.json'), '{"last": "../x"}');
        expect(await lib.lastRun()).toBeUndefined();
        writeFileSync(path.join(lib.dir, 'state.json'), 'garbage');
        expect(await lib.lastRun()).toBeUndefined();
    });

    it('chooses the environment to start with', async () => {
        expect(await lib.startup()).toBe('default');
        expect(files()).toEqual(['default.json']);
        await lib.create('lab');
        await lib.create('aaa');
        expect(await lib.startup()).toBe('aaa');
        await lib.setLastRun('lab');
        expect(await lib.startup()).toBe('lab');
        expect(await lib.startup('default')).toBe('default');
        await refused(lib.startup('nope'), 'Unknown environment: nope');
        await lib.delete('lab');
        expect(await lib.startup()).toBe('aaa');
    });
});
