// hakkas' Air Handling Unit example (hakkas plan X1, examples/ahu/) with the real hakkas against
// the emulated Colibri 8 of examples/ahu/ahu.json: the seasons' regulators, a stopped unit,
// frost protection and Extended operation. The example's calendar settings are adapted to the
// moment the test runs (hakkas has no clock to set): the schedule always on or never, the
// season under test the current month, the pump exercise never. Skipped when hakkas is not
// built (HAKKAS_BIN, else the debug preset's build/debug/hakkas).
import {type ChildProcess, spawn} from 'node:child_process';
import {randomBytes} from 'node:crypto';
import {existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync} from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import {afterEach, beforeEach, describe, expect, it} from 'vitest';
import {loadCatalog} from '../src/catalog.js';
import {Checker, type JsonObject} from '../src/check.js';
import {describe as describeSystem} from '../src/description.js';
import {readEnvironment} from '../src/environment.js';
import {LinkServer} from '../src/link.js';
import {createLogger} from '../src/log.js';
import {BUILTIN_DATA, PACKAGE_ROOT} from '../src/package.js';
import {Scanner} from '../src/scan.js';
import {System, type OutputChannel} from '../src/system.js';

const HAKKAS = process.env.HAKKAS_BIN ?? path.resolve(PACKAGE_ROOT, '../../build/debug/hakkas');
const EXAMPLE = path.resolve(PACKAGE_ROOT, '../../examples/ahu');
const catalog = await loadCatalog([BUILTIN_DATA], createLogger('error', () => {}));

type Initial = {src: {data: unknown}, tgt: {block: string, port: string}};
const example = JSON.parse(readFileSync(path.join(EXAMPLE, 'Air Handling Unit.process.json'), 'utf8'));
const environment = JSON.parse(readFileSync(path.join(EXAMPLE, 'ahu.json'), 'utf8')) as JsonObject;

type Season = 'heating' | 'cooling' | 'neither';

// The example's process with its calendar settings for this test.
function graph(season: Season, running: boolean, extendedMs?: number): JsonObject {
    const g = structuredClone(example.processes[0].graph);
    const month = new Date().getUTCMonth() + 1;           // hakkas' time zone is UTC by default
    const other = String(month % 12 + 1), another = String((month + 1) % 12 + 1);
    const set = (block: string, port: string, data: unknown) => {
        const c = (g.connections as Initial[]).find(x => x.tgt.block === block && x.tgt.port === port && 'data' in x.src);
        expect(c, `${block}.${port}`).toBeDefined();
        c!.src.data = data;
    };
    set('workdays', 'days', '1-7');
    set('work_hours', 'range', running ? '00:00-24:00' : '00:00-00:00');
    set('heating_months', 'months', season === 'heating' ? String(month) : other);
    set('september', 'months', another);
    set('may', 'months', another);
    set('cooling_season', 'months', season === 'cooling' ? String(month) : other);
    set('exercise_time', 'range', '00:00-00:00');
    set('power_up', 'milliseconds', 1000);
    if (extendedMs !== undefined)
        set('extended_wait', 'in_false', extendedMs);
    return g;
}

let tmp: string;
let socketPath: string;
let link: LinkServer;
let scanner: Scanner | undefined;
let system: System;
let hakkas: ChildProcess | undefined;
let hakkasLog: string[];

async function until(condition: () => boolean, what: string, ms = 8000): Promise<void> {
    const end = Date.now() + ms;
    while (!condition()) {
        if (Date.now() > end)
            throw new Error(`timed out waiting for ${what}: ${outputs()}\nhakkas:\n${hakkasLog.join('\n')}`);
        await new Promise(r => setTimeout(r, 10));
    }
}

// The outputs, for a failure's message.
function outputs(): string {
    return ['slot4.aq.1', 'slot4.aq.2', 'slot5.aq.1', 'slot6.dq.1', 'slot6.dq.3', 'slot6.dq.4']
        .map(k => `${k}=${String((system.item(k) as OutputChannel).value)}`).join(' ');
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

// The emulated Colibri 8 of the example, its inputs noiseless at the given temperatures.
function serve(temperatures: Record<string, number>): void {
    const c = new Checker();
    const env = readEnvironment(c, {...environment, inputs: {}})!;
    expect(c.errors).toEqual([]);
    system = System.build('ahu', env, catalog, () => performance.now(), Math.random).system;
    for (const [key, t] of Object.entries(temperatures)) {
        system.set(`${key}.source`, t);
        system.set(`${key}.noise`, 0);
    }
    scanner = new Scanner(system, env, () => performance.now(), createLogger('error', () => {}), t => link.turn(t));
    link.serve({system, scanner, description: describeSystem(system)});
    scanner.start();
}

async function startHakkas(g: JsonObject): Promise<void> {
    const conf = path.join(tmp, 'conf'), data = path.join(tmp, 'data');
    mkdirSync(conf);
    mkdirSync(path.join(data, 'graphs'), {recursive: true});
    mkdirSync(path.join(data, 'bindings'));
    writeFileSync(path.join(conf, 'hakkas.json'), JSON.stringify({
        auth: {server_secret: randomBytes(32).toString('base64')}, environment: {socket: socketPath}}));
    writeFileSync(path.join(data, 'graphs', `${g.id}.json`), JSON.stringify(g));
    writeFileSync(path.join(data, 'bindings', `${g.id}.json`), JSON.stringify(example.processes[0].bindings));
    const port = await freePortPair();
    const proc = spawn(HAKKAS, ['--info', '--confdir', conf, '--basedir', data, 'run',
        '--host', '127.0.0.1', '--port', String(port), '--webdir', tmp], {stdio: ['ignore', 'pipe', 'pipe']});
    hakkas = proc;
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

const out = (key: string) => (system.item(key) as OutputChannel).value;
const volts = (key: string) => out(key) as number;
const press = (input: number, down: boolean) =>
    system.set(`slot7.in.${input}.source`, {pulse: {frequency: 0, duty: down ? 100 : 0}});

// The outputs, by the example's names.
const FAN_S = 'slot6.dq.1', FAN_E = 'slot6.dq.2', PUMP_H = 'slot6.dq.3', PUMP_C = 'slot6.dq.4';
const VALVE_H = 'slot4.aq.1', VALVE_C = 'slot4.aq.2', EXCHANGER = 'slot5.aq.1';
// Outdoor 2 °C (the curve gives 19.8 °C), room at its setpoint; the coil's water warm.
const START = {'slot1.ai.1': 2, 'slot1.ai.2': 19.8, 'slot2.ai.1': 22, 'slot2.ai.2': 6,
    'slot3.ai.1': 30, 'slot3.ai.2': 21};

beforeEach(async () => {
    tmp = mkdtempSync(path.join(os.tmpdir(), 'colibri-ahu-'));
    socketPath = path.join(os.tmpdir(), `ce-${randomBytes(6).toString('hex')}.sock`);
    hakkasLog = [];
    link = new LinkServer(socketPath, createLogger('error', () => {}));
    await link.listen();
});

afterEach(async () => {
    const proc = hakkas;
    hakkas = undefined;
    if (proc !== undefined && proc.exitCode === null) {
        const exited = new Promise(resolve => proc.once('exit', resolve));
        proc.kill('SIGINT');
        await exited;
    }
    scanner?.stop();
    scanner = undefined;
    await link.close();
    rmSync(tmp, {recursive: true, force: true});
});

describe.skipIf(!existsSync(HAKKAS))('the Air Handling Unit example', () => {
    it('loads with all its bindings and makes the DIO1 channels outputs', async () => {
        serve(START);
        await startHakkas(graph('heating', true));
        expect(hakkasLog).toContainEqual(expect.stringMatching(/19 bindings applied from .*, 0 skipped/));
        expect(hakkasLog.filter(l => /^(warn|error)/.test(l) && !/users\.json not found|no frontend in/.test(l)))
            .toEqual([]);
        await until(() => [1, 2, 3, 4].every(i => out(`slot6.dir.${i}`) === true), 'the directions');
    }, 20_000);

    it('in the heating season: the heat exchanger at 100 %, the heating valve regulates', async () => {
        serve({...START, 'slot1.ai.2': 15});
        await startHakkas(graph('heating', true));
        await until(() => out(FAN_S) === true && out(FAN_E) === true, 'the fans');
        expect(out(PUMP_H)).toBe(true);
        expect(out(PUMP_C)).toBe(false);
        await until(() => volts(EXCHANGER) === 10, 'the heat exchanger at 100 %');
        // Too cold: the valve opens from 0 (bumpless) and keeps opening (the integral).
        await until(() => volts(VALVE_H) > 0.2, 'the heating valve to open');
        const first = volts(VALVE_H);
        await until(() => volts(VALVE_H) > first + 0.2, 'the heating valve to open further');
        expect(volts(VALVE_C)).toBe(0);
        // Too warm: it closes.
        system.set('slot1.ai.2.source', 30);
        await until(() => volts(VALVE_H) === 0, 'the heating valve to close');
    }, 30_000);

    it('in neither season: the heat exchanger regulates, the valves and pumps rest', async () => {
        serve({...START, 'slot1.ai.2': 15});
        await startHakkas(graph('neither', true));
        await until(() => out(FAN_S) === true, 'the fans');
        await until(() => volts(EXCHANGER) > 0.2, 'the heat exchanger to recover heat');
        expect(volts(VALVE_H)).toBe(0);
        expect(volts(VALVE_C)).toBe(0);
        expect(out(PUMP_H)).toBe(false);
        expect(out(PUMP_C)).toBe(false);
        system.set('slot1.ai.2.source', 30);
        await until(() => volts(EXCHANGER) === 0, 'the heat exchanger to stop recovering');
    }, 30_000);

    it('in the cooling season: the heat exchanger at 100 %, the cooling valve regulates', async () => {
        serve({...START, 'slot1.ai.1': 25, 'slot1.ai.2': 24});
        await startHakkas(graph('cooling', true));
        await until(() => out(FAN_S) === true, 'the fans');
        expect(out(PUMP_C)).toBe(true);
        expect(out(PUMP_H)).toBe(false);
        await until(() => volts(EXCHANGER) === 10, 'the heat exchanger at 100 %');
        await until(() => volts(VALVE_C) > 0.2, 'the cooling valve to open');
        expect(volts(VALVE_H)).toBe(0);
        system.set('slot1.ai.2.source', 12);
        await until(() => volts(VALVE_C) === 0, 'the cooling valve to close');
    }, 30_000);

    it('outside the schedule: fans, pumps, valves and heat exchanger off', async () => {
        serve({...START, 'slot1.ai.2': 15});
        await startHakkas(graph('heating', false));
        await until(() => out('slot6.dir.1') === true, 'the process to run');
        await new Promise(r => setTimeout(r, 1500));      // a few regulator samples
        expect([out(FAN_S), out(FAN_E), out(PUMP_H), out(PUMP_C)]).toEqual([false, false, false, false]);
        expect([volts(VALVE_H), volts(VALVE_C), volts(EXCHANGER)]).toEqual([0, 0, 0]);
    }, 20_000);

    it('frost: fans stop, the water is held at 20 °C; restart only on an acknowledgement after that',
        async () => {
        serve(START);
        await startHakkas(graph('heating', true));
        await until(() => out(FAN_S) === true, 'the fans');
        system.set('slot3.ai.1.source', 3);
        await until(() => out(FAN_S) === false && out(FAN_E) === false, 'the fans to stop (after 5 s)', 10000);
        expect(out(PUMP_H)).toBe(true);
        await until(() => volts(VALVE_H) > 5, 'the heating valve to open wide', 15000);
        // Acknowledged while still cold: nothing.
        press(2, true);
        await new Promise(r => setTimeout(r, 300));
        press(2, false);
        // Warm again (above 20 °C), past the 5 s confirmation: still locked, the early
        // acknowledgement did not count.
        system.set('slot3.ai.1.source', 25);
        await new Promise(r => setTimeout(r, 7000));
        expect(out(FAN_S)).toBe(false);
        press(2, true);
        await until(() => out(FAN_S) === true && out(FAN_E) === true, 'the fans to restart');
        press(2, false);
    }, 30_000);

    it('Extended operation runs the unit outside the schedule, then stops it', async () => {
        serve(START);
        await startHakkas(graph('heating', false, 1500));
        await until(() => out('slot6.dir.1') === true, 'the process to run');
        await new Promise(r => setTimeout(r, 1500));      // past the start delay
        expect(out(FAN_S)).toBe(false);
        press(1, true);
        const started = Date.now();
        await until(() => out(FAN_S) === true, 'the fans to start');
        press(1, false);
        await until(() => out(FAN_S) === false, 'the fans to stop');
        expect(Date.now() - started).toBeGreaterThan(1000);
        // A new press starts it again.
        press(1, true);
        await until(() => out(FAN_S) === true, 'the fans to start again');
        press(1, false);
    }, 30_000);
});
