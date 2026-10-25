import {mkdtempSync, rmSync, writeFileSync, existsSync} from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import {afterEach, beforeEach, describe, expect, it} from 'vitest';
import {loadCatalog} from '../src/catalog.js';
import {Checker, type JsonObject} from '../src/check.js';
import {describe as describeSystem} from '../src/description.js';
import {readEnvironment} from '../src/environment.js';
import {encode, LINE_MAX, LinkServer, type LinkState, type Served} from '../src/link.js';
import {createLogger} from '../src/log.js';
import {BUILTIN_DATA} from '../src/package.js';
import {Scanner} from '../src/scan.js';
import {System, type OutputChannel} from '../src/system.js';

const catalog = await loadCatalog([BUILTIN_DATA], createLogger('error', () => {}));

let tmp: string;
let socketPath: string;
let lines: string[];
let states: LinkState[];
let notices: string[];
let link: LinkServer;
let now: number;
const clients: net.Socket[] = [];

beforeEach(async () => {
    tmp = mkdtempSync(path.join(os.tmpdir(), 'colibri-link-'));
    socketPath = path.join(tmp, 'run', 'environment.sock');
    lines = [];
    states = [];
    notices = [];
    now = 0;
    link = new LinkServer(socketPath, createLogger('fine', l => lines.push(l)), {
        state: s => states.push(s),
        notice: (level, message) => notices.push(`${level} ${message}`),
    });
    await link.listen();
});

afterEach(async () => {
    for (const c of clients.splice(0))
        c.destroy();
    await link.close();
    rmSync(tmp, {recursive: true, force: true});
});

// A system on a fake clock with its scanner, the scanner's turns going to the link.
function served(env: JsonObject = {}, name = 'lab'): Served & {run(ms: number): void} {
    const c = new Checker();
    const environment = readEnvironment(c, {carrier: 'colibri_8', modules: [
        {slot: 1, type: 'colibri_aiv'}, {slot: 3, type: 'colibri_ssr'}, {slot: 4, type: 'colibri_dp1'}], ...env});
    expect(c.errors).toEqual([]);
    const {system} = System.build(name, environment!, catalog, () => now, () => 0.5);
    const scanner = new Scanner(system, environment!, () => now, createLogger('error', () => {}),
        t => link.turn(t));
    return {
        system, scanner, description: describeSystem(system),
        run(ms: number) {
            for (let i = 0; i < ms; i += 10) {
                now += 10;
                scanner.poll();
            }
        },
    };
}

// A client like hakkas: the messages it got, in order.
interface Client {
    socket: net.Socket;
    messages: JsonObject[];
    closed: Promise<void>;
    next(n?: number): Promise<JsonObject[]>;
    send(m: object | string): void;
}

function connect(): Promise<Client> {
    return new Promise((resolve, reject) => {
        const socket = net.connect(socketPath);
        clients.push(socket);
        const messages: JsonObject[] = [];
        let read = 0;
        let buffer = '';
        let waiting: (() => void) | undefined;
        socket.on('data', d => {
            buffer += d.toString();
            let nl;
            while ((nl = buffer.indexOf('\n')) >= 0) {
                messages.push(JSON.parse(buffer.slice(0, nl)));
                buffer = buffer.slice(nl + 1);
            }
            waiting?.();
        });
        const closed = new Promise<void>(r => socket.on('close', () => {
            r();
            waiting?.();
        }));
        const client: Client = {
            socket, messages, closed,
            // The next n messages.
            async next(n = 1) {
                while (messages.length < read + n && !socket.destroyed)
                    await new Promise<void>(r => waiting = r);
                const out = messages.slice(read, read + n);
                read += out.length;
                return out;
            },
            send(m) {
                socket.write((typeof m === 'string' ? m : JSON.stringify(m)) + '\n');
            },
        };
        socket.once('connect', () => resolve(client));
        socket.once('error', reject);
    });
}

const settle = () => new Promise(r => setTimeout(r, 30));
const warnings = () => lines.filter(l => l.startsWith('warn '));

describe('the environment link', () => {
    it('listens on its socket, making the directory, and removes the file when closed', async () => {
        expect(existsSync(socketPath)).toBe(true);
        expect(lines).toEqual([`info environment link: listening on ${socketPath}`]);
        await link.close();
        expect(existsSync(socketPath)).toBe(false);
    });

    it('replaces a stale socket file, and refuses a live one or another file', async () => {
        const stale = path.join(tmp, 'stale.sock');
        const s = net.createServer().listen(stale);
        await new Promise(r => s.once('listening', r));
        const other = new LinkServer(stale, createLogger('error', () => {}));
        await expect(other.listen()).rejects.toThrow(`${stale}: another program is listening on it`);
        // A server that ended without removing its file leaves it stale.
        await new Promise(r => s.close(r));
        writeFileSync(path.join(tmp, 'plain'), '');
        await expect(new LinkServer(path.join(tmp, 'plain'), createLogger('error', () => {})).listen())
            .rejects.toThrow('exists and is not a socket');
    });

    it('sends the description, then every current sensor value, then the changes at each turn', async () => {
        const s = served({inputs: {'slot1.ai.1': {source: {ramp: {from: 0, to: 10, period: 1, repeat: false}}}}});
        link.serve(s);
        s.scanner.poll();
        s.run(40);                  // slots 0-4: slots 1 and 4 have their first values
        const c = await connect();
        const [describe] = await c.next();
        expect(describe).toEqual({describe: JSON.parse(JSON.stringify(s.description))});
        const initial = await c.next(3);
        expect(initial.map(m => (m.value as JsonObject).port + ':' + (m.value as JsonObject).index)).toEqual([
            'slot1-ai:1', 'slot1-ai:2', 'slot4-ai:undefined',
        ]);
        expect(initial[2]).toEqual({value: {port: 'slot4-ai', value: (s.system.item('slot4.ai') as {value: number}).value}});
        expect(states).toEqual(['connected']);
        s.run(80);                  // slot 1 again (at 90 ms): only the ramp changed
        const ai1 = s.system.item('slot1.ai.1') as {value: number};
        expect(ai1.value).toBeCloseTo(0.9, 2);
        expect(await c.next()).toEqual([{value: {port: 'slot1-ai', index: 1, value: ai1.value}}]);
    });

    it('queues hakkas\' writes for the module\'s turn', async () => {
        const s = served();
        link.serve(s);
        s.scanner.poll();
        const c = await connect();
        await c.next();
        c.send({hello: {version: 1}});
        c.send({write: {port: 'slot3-dq', index: 2, value: true}});
        await settle();
        const dq2 = s.system.item('slot3.dq.2') as OutputChannel;
        expect(dq2.value).toBe(false);
        s.run(30);
        expect(dq2.value).toBe(true);
        expect(s.scanner.stats().channels['slot3.dq.2']).toEqual({received: 1, queued: 1, dropped: 0});
        expect(warnings()).toEqual([]);
    });

    it('warns about bad lines and writes, at most 8 per connection, and ignores unknown messages', async () => {
        const s = served();
        link.serve(s);
        const c = await connect();
        await c.next();
        for (const m of [
            'nonsense', '[1]', '{"a": 1, "b": 2}', {future: {x: 1}},
            {write: {index: 1, value: true}},
            {write: {port: 'slot9-dq', index: 1, value: true}},
            {write: {port: 'slot1-ai', index: 1, value: 5}},
            {write: {port: 'slot3-dq', index: 5, value: true}},
            {write: {port: 'slot3-dq', value: true}},
            {write: {port: 'slot3-dq', index: 1, value: 1}},
            {write: {port: 'slot3-dq', index: 1, value: 'on'}},
        ])
            c.send(m);
        await settle();
        expect(warnings()).toEqual([
            'warn environment link: not JSON, ignored: nonsense',
            'warn environment link: not an object with one member, ignored: [1]',
            'warn environment link: not an object with one member, ignored: {"a": 1, "b": 2}',
            'warn environment link: write: expected {port, index, value}, ignored',
            'warn environment link: write: unknown port slot9-dq, ignored',
            'warn environment link: write: slot1-ai is a sensor port, ignored',
            'warn environment link: write: slot3-dq has no channel 5, ignored',
            'warn environment link: write: slot3-dq has no channel (none), ignored',
        ]);
        expect(c.socket.destroyed).toBe(false);
        expect(s.scanner.stats().channels['slot3.dq.1']).toEqual({received: 0, queued: 0, dropped: 0});
        // A new connection may warn again.
        c.socket.destroy();
        await settle();
        const d = await connect();
        await d.next();
        d.send({write: {port: 'slot4-ai', value: 1}});
        await settle();
        expect(warnings().at(-1)).toBe('warn environment link: write: slot4-ai is a sensor port, ignored');
    });

    it('takes a number as null for NaN, and a plain port\'s write without an index', async () => {
        const s = served({modules: [{slot: 1, type: 'colibri_aqv'}]});
        link.serve(s);
        s.scanner.poll();
        const c = await connect();
        await c.next();
        const port = s.description.inPorts[0]!;
        c.send(port.addressable ? {write: {port: port.id, index: 1, value: null}} : {write: {port: port.id, value: null}});
        await settle();
        s.run(20);
        expect((s.system.portChannels(port.id)![0] as OutputChannel).value).toBeNaN();
        expect(warnings()).toEqual([]);
    });

    it('closes a connection with another version, or a line over 256 KiB', async () => {
        link.serve(served());
        let c = await connect();
        c.send({hello: {version: 2}});
        await c.closed;
        expect(warnings()).toEqual(['warn environment link: hello: version 2, expected 1: connection closed']);
        c = await connect();
        c.send('{"write": "' + 'x'.repeat(LINE_MAX) + '"}');
        await c.closed;
        expect(warnings()[1]).toBe(`warn environment link: a line longer than ${LINE_MAX} bytes: connection closed`);
        // Exactly at the limit is a line.
        c = await connect();
        const line = '{"future": "' + 'x'.repeat(LINE_MAX - 16) + '"}';
        expect(Buffer.byteLength(line) + 1).toBe(LINE_MAX - 1);
        c.send(line);
        await settle();
        expect(c.socket.destroyed).toBe(false);
    });

    it('takes one hakkas at a time: another is closed at once, with a notice', async () => {
        link.serve(served());
        const a = await connect();
        await a.next();
        const b = await connect();
        await b.closed;
        expect(b.messages).toEqual([]);
        expect(notices).toEqual(['warning a second runtime connected: closed (one hakkas per emulator)']);
        expect(a.socket.destroyed).toBe(false);
        // Once the first has gone, the next one is described.
        a.socket.destroy();
        await settle();
        const c = await connect();
        expect(Object.keys((await c.next())[0]!)).toEqual(['describe']);
        expect(states).toEqual(['connected', 'waiting', 'connected']);
    });

    it('closes connections while there is no environment to describe, warning once', async () => {
        for (let i = 0; i < 2; i++) {
            const c = await connect();
            await c.closed;
            expect(c.messages).toEqual([]);
        }
        expect(warnings()).toEqual(['warn environment link: no environment to describe: connections are closed']);
    });

    it('closes the link at a power cycle, and is restart until a connection stays 2 s', async () => {
        link.serve(served());
        const a = await connect();
        await a.next();
        link.serve(served({modules: [{slot: 2, type: 'colibri_ssr'}]}));
        await a.closed;
        expect(states).toEqual(['connected', 'restart']);
        // hakkas, seeing another description, closes at once: still restart.
        const b = await connect();
        expect((await b.next())[0]!.describe).toMatchObject({inPorts: [{id: 'slot2-dq'}]});
        b.socket.destroy();
        await settle();
        expect(states).toEqual(['connected', 'restart']);
        // A restarted hakkas stays.
        const c = await connect();
        await c.next();
        await new Promise(r => setTimeout(r, 2100));
        expect(states).toEqual(['connected', 'restart', 'connected']);
    }, 5000);

    it('takes a reconnection at once after a power cycle, before the old socket has closed', async () => {
        link.serve(served());
        const a = await connect();
        await a.next();
        link.serve(served({modules: [{slot: 2, type: 'colibri_ssr'}]}));
        const b = await connect();      // no wait for the old connection's close
        expect(Object.keys((await b.next())[0]!)).toEqual(['describe']);
        expect(notices).toEqual([]);
    });

    it('is no power cycle when no hakkas had the description', async () => {
        link.serve(served());
        link.serve(served({modules: []}));
        expect(states).toEqual([]);
    });
});

describe('encoding a message', () => {
    it('writes a NaN as null and an infinity as 1e999', () => {
        expect(encode({value: {port: 'p', value: NaN}})).toBe('{"value":{"port":"p","value":null}}');
        expect(encode({a: Infinity, b: -Infinity, c: 1.5})).toBe('{"a":1e999,"b":-1e999,"c":1.5}');
        expect(JSON.parse(encode({a: -Infinity})).a).toBe(-Infinity);
    });
});
