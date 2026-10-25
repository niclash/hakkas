// The emulator with the real hakkas (plan task E8): hakkas' POSIX build connects to the
// emulator's environment link, binds a process to an AIV input and an SSR output, and values
// travel both ways through the scan cycle. Skipped when hakkas is not built (HAKKAS_BIN, else
// the debug preset's build/debug/hakkas).
import {type ChildProcess, spawn} from 'node:child_process';
import {randomBytes} from 'node:crypto';
import {existsSync, mkdirSync, mkdtempSync, rmSync, writeFileSync} from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import {afterEach, beforeEach, describe, expect, it} from 'vitest';
import {loadCatalog} from '../src/catalog.js';
import {Checker, type JsonObject} from '../src/check.js';
import {describe as describeSystem} from '../src/description.js';
import {runEmulator} from '../src/emulator.js';
import {readEnvironment} from '../src/environment.js';
import {Library} from '../src/library.js';
import {LinkServer, type Served} from '../src/link.js';
import {createLogger} from '../src/log.js';
import {BUILTIN_DATA, PACKAGE_ROOT} from '../src/package.js';
import {Scanner, type Turn} from '../src/scan.js';
import {System, type OutputChannel} from '../src/system.js';

const HAKKAS = process.env.HAKKAS_BIN ?? path.resolve(PACKAGE_ROOT, '../../build/debug/hakkas');
const catalog = await loadCatalog([BUILTIN_DATA], createLogger('error', () => {}));
const clock = () => performance.now();

// The process: out = slot1-ai[1] > 5, driving slot3-dq[1].
const GRAPH = {
    id: 'threshold', name: 'Threshold', description: '', icon: '', started: true,
    blocks: {gt: {component: 'compare/Greater', metadata: {x: 0, y: 0}}},
    connections: [{src: {data: 5}, tgt: {block: 'gt', port: 'in2'}}],
    inports: [], outports: [],
};
const BINDINGS = {environment: 'lab', bindings: [
    {block: 'gt', port: 'in1', env: 'slot1-ai', index: 1},
    {block: 'gt', port: 'out', env: 'slot3-dq', index: 1},
]};

let tmp: string;
let socketPath: string;
let link: LinkServer;
let hakkas: ChildProcess | undefined;
let hakkasLog: string[];
let emulatorLog: string[];
let turns: Turn[];
const scanners: Scanner[] = [];

// The emulator's side, as runEmulator builds it, with every turn recorded.
function serve(modules: JsonObject[]): Served {
    const c = new Checker();
    const env = readEnvironment(c, {carrier: 'colibri_8', modules})!;
    expect(c.errors).toEqual([]);
    const {system} = System.build('lab', env, catalog, clock, Math.random);
    const scanner = new Scanner(system, env, clock, createLogger('error', () => {}), t => {
        turns.push(t);
        link.turn(t);
    });
    scanners.push(scanner);
    const served = {system, scanner, description: describeSystem(system)};
    link.serve(served);
    scanner.start();
    return served;
}

async function startLink(): Promise<void> {
    link = new LinkServer(socketPath, createLogger('fine', l => emulatorLog.push(l)));
    await link.listen();
}

async function freePortPair(): Promise<number> {
    const free = (port: number) => new Promise<boolean>(resolve => {
        const s = net.createServer().once('error', () => resolve(false))
            .listen(port, '127.0.0.1', () => s.close(() => resolve(true)));
    });
    for (;;) {
        const port = 20000 + Math.floor(Math.random() * 40000);
        if (await free(port) && await free(port + 1))
            return port;
    }
}

async function startHakkas(): Promise<void> {
    const conf = path.join(tmp, 'conf'), data = path.join(tmp, 'data');
    mkdirSync(conf);
    mkdirSync(path.join(data, 'graphs'), {recursive: true});
    mkdirSync(path.join(data, 'bindings'));
    writeFileSync(path.join(conf, 'hakkas.json'), JSON.stringify({
        auth: {server_secret: randomBytes(32).toString('base64')}, environment: {socket: socketPath},
    }));
    writeFileSync(path.join(data, 'graphs', 'threshold.json'), JSON.stringify(GRAPH));
    writeFileSync(path.join(data, 'bindings', 'threshold.json'), JSON.stringify(BINDINGS));
    const port = await freePortPair();
    const proc = spawn(HAKKAS, ['--info', '--confdir', conf, '--basedir', data, 'run',
        '--host', '127.0.0.1', '--port', String(port), '--webdir', tmp], {stdio: ['ignore', 'pipe', 'pipe']});
    hakkas = proc;
    // Whole lines: a chunk may end in the middle of one.
    let partial = '';
    const collect = (d: Buffer) => {
        const lines = (partial + d.toString()).split('\n');
        partial = lines.pop()!;
        hakkasLog.push(...lines.filter(l => l !== ''));
    };
    proc.stdout!.on('data', collect);
    proc.stderr!.on('data', collect);
    await until(() => hakkasLog.some(l => / process(es)? loaded/.test(l)), 'hakkas to start');
}

async function stopHakkas(): Promise<number | null> {
    const proc = hakkas;
    hakkas = undefined;
    if (proc === undefined || proc.exitCode !== null)
        return proc?.exitCode ?? null;
    const exited = new Promise<number | null>(resolve => proc.once('exit', code => resolve(code)));
    proc.kill('SIGINT');
    const timer = setTimeout(() => proc.kill('SIGKILL'), 5000);
    const code = await exited;
    clearTimeout(timer);
    return code;
}

async function until(condition: () => boolean, what: string, ms = 5000): Promise<void> {
    const end = Date.now() + ms;
    while (!condition()) {
        if (Date.now() > end)
            throw new Error(`timed out waiting for ${what}\nhakkas:\n${hakkasLog.join('\n')}\n` +
                `emulator:\n${emulatorLog.join('\n')}`);
        await new Promise(r => setTimeout(r, 5));
    }
}

// hakkas' warnings and errors, but the two of this setup: no users file (no login needed), and
// a web directory without the frontend.
const problems = () => hakkasLog.filter(l => /^(warn|error)/.test(l) && !/users\.json not found|no frontend in/.test(l));

const writes = (s: Served, key: string) =>
    (s.scanner.stats().channels[key] as {received: number}).received;

beforeEach(async () => {
    tmp = mkdtempSync(path.join(os.tmpdir(), 'colibri-hakkas-'));
    // A Unix socket's path is short: directly in the temporary directory.
    socketPath = path.join(os.tmpdir(), `ce-${randomBytes(6).toString('hex')}.sock`);
    hakkasLog = [];
    emulatorLog = [];
    turns = [];
    await startLink();
});

afterEach(async () => {
    await stopHakkas();
    for (const s of scanners.splice(0))
        s.stop();
    await link.close();
    rmSync(tmp, {recursive: true, force: true});
});

describe.skipIf(!existsSync(HAKKAS))('the emulator with hakkas', () => {
    it('is accepted, and carries values both ways through the scan cycle', async () => {
        const s = serve([{slot: 1, type: 'colibri_aiv'}, {slot: 3, type: 'colibri_ssr'}]);
        s.system.set('slot1.ai.1.source', 2);
        await startHakkas();
        expect(hakkasLog).toContainEqual(expect.stringMatching(/process threshold: 2 bindings applied from .*, 0 skipped/));
        expect(problems()).toEqual([]);
        expect(link.state).toBe('connected');
        // At its start the process drives its output: false, as 2 > 5 is.
        const dq1 = s.system.item('slot3.dq.1') as OutputChannel;
        await until(() => writes(s, 'slot3.dq.1') >= 1, 'the first write');

        // The sensor's change reaches hakkas at slot 1's turn, its result the relay at slot 3's.
        s.system.set('slot1.ai.1.source', 8);
        const from = turns.length;
        await until(() => dq1.value === true, 'the relay to switch');
        const later = turns.slice(from);
        const sampled = later.findIndex(t => t.slot.index === 1 && t.changed.some(ch => ch.key === 'slot1.ai.1'));
        // (The process's first write may be applied in these turns too, before the sample.)
        const applied = later.findIndex((t, i) => i > sampled && t.applied.some(ch => ch.key === 'slot3.dq.1'));
        expect(sampled).toBeGreaterThanOrEqual(0);
        expect(applied).toBeGreaterThan(sampled);
        expect(later[applied]!.slot.index).toBe(3);
        expect(applied - sampled).toBeLessThanOrEqual(2 + 8);   // slot 3's next turn, or the one after

        s.system.set('slot1.ai.1.source', 1);
        await until(() => dq1.value === false, 'the relay to switch back');
        expect(await stopHakkas()).toBe(0);
    }, 20_000);

    it('after a reconnect, gets the sensor values again and its actuators written again', async () => {
        const s = serve([{slot: 1, type: 'colibri_aiv'}, {slot: 3, type: 'colibri_ssr'}]);
        s.system.set('slot1.ai.1.source', 8);
        await startHakkas();
        const dq1 = s.system.item('slot3.dq.1') as OutputChannel;
        await until(() => dq1.value === true, 'the relay to switch');
        const before = writes(s, 'slot3.dq.1');

        // The emulator's link goes away and comes back with the same description.
        await link.close();
        await until(() => hakkasLog.some(l => /environment at .* disconnected/.test(l)), 'hakkas to notice');
        await startLink();
        link.serve(s);
        await until(() => link.state === 'connected', 'hakkas to reconnect', 5000);
        await until(() => hakkasLog.some(l => /environment at .* reconnected/.test(l)), 'hakkas to log it');
        await until(() => writes(s, 'slot3.dq.1') > before, 'the actuator re-written');
        expect(dq1.value).toBe(true);
        // The sensor values were sent again; a change still gets through.
        s.system.set('slot1.ai.1.source', 1);
        await until(() => dq1.value === false, 'the relay to switch back');
        expect(problems().filter(l => l.startsWith('error'))).toEqual([]);
        expect(await stopHakkas()).toBe(0);
    }, 20_000);

    it('runs with the whole emulator, driven over its control socket like a script', async () => {
        // The emulator as the command runs it, with its own link in place of the test's.
        await link.close();
        const library = new Library(path.join(tmp, 'library'));
        await library.init();
        writeFileSync(library.file('lab'), JSON.stringify({carrier: 'colibri_8',
            modules: [{slot: 1, type: 'colibri_aiv'}, {slot: 3, type: 'colibri_ssr'}]}));
        const controlSocket = path.join(os.tmpdir(), `cc-${randomBytes(6).toString('hex')}.sock`);
        const stop = new AbortController();
        const running = runEmulator({library: library.dir, plugins: [], socket: socketPath, controlSocket, httpPort: 0,
            seed: 1, logLevel: 'fine', environment: 'lab'}, createLogger('fine', l => emulatorLog.push(l)), stop.signal);
        try {
            await until(() => emulatorLog.some(l => l.startsWith('info http: listening')), 'the emulator to start');
            await startHakkas();
            const socket = net.connect(controlSocket);
            const state: JsonObject = {};
            let buffer = '';
            socket.on('data', d => {
                buffer += d.toString();
                let nl;
                while ((nl = buffer.indexOf('\n')) >= 0) {
                    const m = JSON.parse(buffer.slice(0, nl)) as JsonObject;
                    buffer = buffer.slice(nl + 1);
                    if ('state' in m)
                        Object.assign(state, (m.state as JsonObject).values);
                    if ('link' in m)
                        state.link = (m.link as JsonObject).state!;
                }
            });
            await until(() => state.link === 'connected', 'the link to be connected');
            socket.write(JSON.stringify({set: {key: 'slot1.ai.1.source', value: 8}}) + '\n');
            await until(() => state['slot3.dq.1'] === true, 'the relay to switch on');
            socket.write(JSON.stringify({set: {key: 'slot1.ai.1.source', value: {square: {low: 0, high: 10,
                period: 1, duty: 50}}}}) + '\n');
            await until(() => state['slot3.dq.1'] === false, 'the relay to follow the square wave off', 3000);
            await until(() => state['slot3.dq.1'] === true, 'and on again', 3000);
            socket.destroy();
            expect(await stopHakkas()).toBe(0);
        } finally {
            stop.abort();
            await running;
            await startLink();     // for afterEach
        }
    }, 20_000);

    it('after a power cycle with another description, hakkas stops its IO until restarted', async () => {
        serve([{slot: 1, type: 'colibri_aiv'}, {slot: 3, type: 'colibri_ssr'}]);
        await startHakkas();
        await until(() => link.state === 'connected', 'hakkas to connect');
        // An SSR moved to slot 4: another description.
        serve([{slot: 1, type: 'colibri_aiv'}, {slot: 4, type: 'colibri_ssr'}]);
        expect(link.state).toBe('restart');
        await until(() => hakkasLog.some(l => /changed its description: no environment IO until hakkas is restarted/.test(l)),
            'hakkas to refuse the new description', 5000);
        expect(link.state).toBe('restart');
        expect(await stopHakkas()).toBe(0);
    }, 20_000);
});
