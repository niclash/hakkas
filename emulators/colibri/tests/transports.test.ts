import {mkdirSync, mkdtempSync, rmSync, writeFileSync} from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import {afterEach, beforeEach, describe, expect, it} from 'vitest';
import {WebSocket} from 'ws';
import {loadCatalog, type Catalog} from '../src/catalog.js';
import type {JsonObject} from '../src/check.js';
import {Core} from '../src/core.js';
import type {ModuleType} from '../src/descriptors.js';
import {Library} from '../src/library.js';
import {LinkServer} from '../src/link.js';
import {createLogger} from '../src/log.js';
import {BUILTIN_DATA} from '../src/package.js';
import {ControlSocket, HttpServer, MESSAGE_MAX} from '../src/transports.js';

const quiet = createLogger('error', () => {});

let tmp: string;
let catalog: Catalog;
let core: Core;
let control: ControlSocket;
let http: HttpServer;

beforeEach(async () => {
    tmp = mkdtempSync(path.join(os.tmpdir(), 'colibri-transports-'));
    catalog = await loadCatalog([BUILTIN_DATA], quiet);
    // A module with a faceplate, its picture in a directory of its own.
    const plugin = path.join(tmp, 'plugin');
    mkdirSync(plugin);
    writeFileSync(path.join(plugin, 'face.svg'), '<svg xmlns="http://www.w3.org/2000/svg"/>');
    writeFileSync(path.join(plugin, 'secret.txt'), 'no');
    const ssr = catalog.modules.get('colibri_ssr')!;
    catalog.modules.set('colibri_ssr', {dir: plugin, def: {...ssr.def, faceplate: 'face.svg'} as ModuleType});
    const ui = path.join(tmp, 'ui');
    mkdirSync(path.join(ui, 'js'), {recursive: true});
    writeFileSync(path.join(ui, 'index.html'), '<!doctype html><title>UI</title>');
    writeFileSync(path.join(ui, 'js', 'app.js'), 'console.log(1);');
    writeFileSync(path.join(tmp, 'outside.txt'), 'outside');
    const library = new Library(path.join(tmp, 'lib'));
    await library.init();
    writeFileSync(library.file('lab'), JSON.stringify({carrier: 'colibri_8', modules: [{slot: 3, type: 'colibri_ssr'}]}));
    core = new Core(catalog, library, new LinkServer(path.join(tmp, 'link.sock'), quiet), () => performance.now(),
        () => 0.5, quiet);
    await core.start('lab');
    control = new ControlSocket(path.join(tmp, 'run', 'control.sock'), core, quiet);
    await control.listen();
    http = new HttpServer(0, core, catalog, ui, quiet);
    await http.listen();
});

afterEach(async () => {
    await http.close();
    await control.close();
    core.stop();
    rmSync(tmp, {recursive: true, force: true});
});

// Reads newline-delimited messages from a socket.
function lines(socket: net.Socket): {messages: JsonObject[], until(test: (m: JsonObject) => boolean): Promise<JsonObject>} {
    const messages: JsonObject[] = [];
    let buffer = '';
    const waiters: (() => void)[] = [];
    socket.on('data', d => {
        buffer += d.toString();
        let nl;
        while ((nl = buffer.indexOf('\n')) >= 0) {
            messages.push(JSON.parse(buffer.slice(0, nl)));
            buffer = buffer.slice(nl + 1);
        }
        waiters.splice(0).forEach(w => w());
    });
    return {
        messages,
        async until(test) {
            for (;;) {
                const m = messages.find(test);
                if (m !== undefined)
                    return m;
                await new Promise<void>(r => waiters.push(r));
            }
        },
    };
}

const get = (p: string, init?: RequestInit) => fetch(`http://127.0.0.1:${http.port}${p}`, init);

describe('the control socket', () => {
    it('speaks the protocol one message per line', async () => {
        const socket = net.connect(control.socketPath);
        const r = lines(socket);
        await r.until(m => 'link' in m);
        expect(Object.keys(r.messages[0]!)).toEqual(['hello']);
        socket.write(JSON.stringify({set: {id: 1, key: 'slot3.dq.1.source', value: true}}) + '\n' +
            JSON.stringify({resetstats: {id: 2}}) + '\n');
        expect(await r.until(m => 'error' in m)).toEqual({error: {id: 1, request: 'set', message: 'Not settable: slot3.dq.1.source'}});
        expect(await r.until(m => 'ok' in m)).toEqual({ok: {id: 2}});
        socket.destroy();
    });

    it('closes a connection whose line is longer than 256 KiB', async () => {
        const socket = net.connect(control.socketPath);
        await new Promise(r => socket.once('connect', r));
        socket.on('error', () => {});
        socket.resume();        // what it is sent is read, so its end is seen
        const closed = new Promise(r => socket.once('close', r));
        socket.write('x'.repeat(MESSAGE_MAX + 10));
        await closed;
    });

    it('refuses to start on a socket another emulator listens on, and leaves its file', async () => {
        const other = new ControlSocket(control.socketPath, core, quiet);
        await expect(other.listen()).rejects.toThrow('another program is listening on it');
        await other.close();
        const socket = net.connect(control.socketPath);
        await new Promise((resolve, reject) => socket.once('connect', resolve).once('error', reject));
        socket.destroy();
    });
});

describe('the HTTP server', () => {
    it('serves the UI\'s files', async () => {
        const index = await get('/');
        expect(index.status).toBe(200);
        expect(index.headers.get('content-type')).toBe('text/html; charset=utf-8');
        expect(await index.text()).toContain('<title>UI</title>');
        const js = await get('/js/app.js');
        expect(js.headers.get('content-type')).toBe('text/javascript; charset=utf-8');
        expect((await get('/nope.js')).status).toBe(404);
        expect((await get('/', {method: 'POST'})).status).toBe(405);
    });

    it('serves a module\'s picture, and nothing else of its directory or outside', async () => {
        const svg = await get('/assets/modules/colibri_ssr/face.svg');
        expect(svg.status).toBe(200);
        expect(svg.headers.get('content-type')).toBe('image/svg+xml');
        for (const p of ['/assets/modules/colibri_ssr/secret.txt', '/assets/modules/colibri_nope/face.svg',
            '/assets/modules/colibri_ssr/../../outside.txt', '/%2e%2e/outside.txt', '/js/%2e%2e/%2e%2e/outside.txt',
            '/assets/modules/colibri_ssr/%2e%2e%2fsecret.txt'])
            expect((await get(p)).status, p).toBe(404);
    });

    it('serves the render copies of the carrier, the modules, the communication and MCU modules', async () => {
        for (const p of ['/assets/carriers/colibri_8/picture.webp', '/assets/modules/colibri_aiv/picture.webp',
            '/assets/comm/colibri_ethernet/picture.webp', '/assets/mcu/colibri_mcu6/picture.webp']) {
            const r = await get(p);
            expect(r.status, p).toBe(200);
            expect(r.headers.get('content-type'), p).toBe('image/webp');
        }
        expect((await get('/assets/mcu/colibri_mcu6/mcu.json')).status).toBe(404);
        expect((await get('/assets/comm/nope/picture.webp')).status).toBe(404);
    });

    it('says the UI is not built when it is not', async () => {
        const bare = new HttpServer(0, core, catalog, path.join(tmp, 'none'), quiet);
        await bare.listen();
        const r = await fetch(`http://127.0.0.1:${bare.port}/`);
        expect(await r.text()).toContain('The UI is not built');
        await bare.close();
    });
});

describe('the WebSocket', () => {
    const open = (origin?: string) => new Promise<WebSocket>((resolve, reject) => {
        const ws = new WebSocket(`ws://127.0.0.1:${http.port}/control`, origin === undefined ? {} : {origin});
        const got: JsonObject[] = [];
        ws.on('message', d => got.push(JSON.parse(d.toString())));
        received.set(ws, got);
        ws.once('open', () => resolve(ws));
        ws.once('unexpected-response', (_, res) => reject(new Error(`HTTP ${res.statusCode}`)));
        ws.once('error', reject);
    });
    // What each socket received, from its first message on.
    const received = new Map<WebSocket, JsonObject[]>();
    const messages = (ws: WebSocket) => received.get(ws)!;

    it('speaks the protocol, one message per frame', async () => {
        const ws = await open();
        const got = messages(ws);
        ws.send(JSON.stringify({set: {id: 'a', key: 'slot0.x.source', value: 1}}));
        await new Promise(r => setTimeout(r, 100));
        expect(Object.keys(got[0]!)).toEqual(['hello']);
        expect(got.find(m => 'error' in m)).toEqual({error: {id: 'a', request: 'set', message: 'Unknown key: slot0.x.source'}});
        ws.close();
    });

    it('takes the page\'s own origin or none, and refuses another', async () => {
        (await open(`http://localhost:${http.port}`)).close();
        (await open(`http://127.0.0.1:${http.port}`)).close();
        await expect(open('http://evil.example')).rejects.toThrow('HTTP 403');
        await expect(open(`http://localhost:${http.port + 1}`)).rejects.toThrow('HTTP 403');
        await expect(new Promise((resolve, reject) => {
            const ws = new WebSocket(`ws://127.0.0.1:${http.port}/other`);
            ws.once('open', resolve);
            ws.once('unexpected-response', (_, res) => reject(new Error(`HTTP ${res.statusCode}`)));
        })).rejects.toThrow('HTTP 404');
    });

    it('closes with 1009 on a message over 256 KiB', async () => {
        const ws = await open();
        const closed = new Promise<number>(r => ws.once('close', code => r(code)));
        ws.send('x'.repeat(MESSAGE_MAX + 1));
        expect(await closed).toBe(1009);
    });

    it('sees a change another client makes', async () => {
        const a = await open(), b = await open();
        const got = messages(b);
        a.send(JSON.stringify({set: {key: 'slot3.dq.1.source', value: 1}}));
        a.send(JSON.stringify({configure: {values: {cycleMs: 25}}}));
        await new Promise(r => setTimeout(r, 100));
        const s = got.filter(m => 'settings' in m).at(-1)!.settings as JsonObject;
        expect((s.values as JsonObject).cycleMs).toBe(25);
        a.close();
        b.close();
    });
});
