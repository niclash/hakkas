// The built command, run as a process.
import {execFileSync, spawn} from 'node:child_process';
import {existsSync, mkdtempSync, rmSync} from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {afterAll, beforeAll, describe, expect, it} from 'vitest';
import {PACKAGE_ROOT, VERSION} from '../src/package.js';

const main = path.join(PACKAGE_ROOT, 'dist', 'main.js');
let tmp: string;

beforeAll(() => {
    execFileSync(process.execPath, [path.join(PACKAGE_ROOT, 'node_modules', 'typescript', 'bin', 'tsc'),
        '-p', path.join(PACKAGE_ROOT, 'tsconfig.build.json')]);
    tmp = mkdtempSync(path.join(os.tmpdir(), 'colibri-main-'));
}, 60_000);

afterAll(() => rmSync(tmp, {recursive: true, force: true}));

interface Result {
    code: number | null;
    stdout: string;
    stderr: string;
}

// Runs the command; `whileRunning` is called with what it has logged, each time it logs.
function command(args: string[], whileRunning?: (pid: number, stderr: string) => void): Promise<Result> {
    return new Promise((resolve, reject) => {
        const p = spawn(process.execPath, [main, ...args], {env: {HOME: tmp, XDG_RUNTIME_DIR: tmp}});
        let stdout = '', stderr = '';
        p.stdout.on('data', d => stdout += d);
        p.stderr.on('data', d => {
            stderr += d;
            whileRunning?.(p.pid!, stderr);
        });
        p.on('error', reject);
        p.on('close', code => resolve({code, stdout, stderr}));
    });
}

describe('colibri-emulator', () => {
    it('prints its usage', async () => {
        const r = await command(['--help']);
        expect(r.code).toBe(0);
        expect(r.stdout).toMatch(/^usage: colibri-emulator/);
    });

    it('prints its version', async () => {
        const r = await command(['--version']);
        expect(r).toEqual({code: 0, stdout: `colibri-emulator ${VERSION}\n`, stderr: ''});
    });

    it('exits 2 on a usage error', async () => {
        const r = await command(['--http', 'x']);
        expect(r.code).toBe(2);
        expect(r.stderr).toMatch(/^colibri-emulator: invalid --http port: x\nusage:/);
    });

    it('exits 1 when the environment named is not in the library', async () => {
        const r = await command(['nope']);
        expect(r.code).toBe(1);
        expect(r.stderr).toMatch(/\nerror Unknown environment: nope\nerror the emulator did not start\n$/);
    });

    for (const signal of ['SIGINT', 'SIGTERM'] as const)
        it(`runs until ${signal}`, async () => {
            let sent = false;
            const r = await command(['--http=0'], (pid, stderr) => {
                if (!sent && stderr.includes('http: listening')) {
                    sent = true;
                    process.kill(pid, signal);
                }
            });
            expect(r.code).toBe(0);
            expect(r.stderr).toMatch(new RegExp(`^info colibri-emulator ${VERSION}: library .*\n` +
                'info catalog: module types 18, carrier types 1, communication module types 4, MCU module types 3\n' +
                'info environment default: carrier colibri_8, modules 0, cycle 10 ms\n' +
                `info environment link: listening on ${tmp}/colibri-emulator/environment.sock\n` +
                `info control: listening on ${tmp}/colibri-emulator/control.sock\n` +
                'info http: listening on http://localhost:[0-9]+/\n' +
                'info stopped\n$'));
            expect(existsSync(path.join(tmp, 'colibri-emulator', 'environment.sock'))).toBe(false);
            expect(existsSync(path.join(tmp, 'colibri-emulator', 'control.sock'))).toBe(false);
        });
});
