import {mkdtempSync, readFileSync, rmSync, writeFileSync} from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {afterEach, beforeEach, describe, expect, it, vi} from 'vitest';
import {loadCatalog} from '../src/catalog.js';
import type {JsonObject} from '../src/check.js';
import {Core, type Client} from '../src/core.js';
import {Library} from '../src/library.js';
import {LinkServer} from '../src/link.js';
import {createLogger} from '../src/log.js';
import {BUILTIN_DATA, VERSION} from '../src/package.js';

const catalog = await loadCatalog([BUILTIN_DATA], createLogger('error', () => {}));
const quiet = createLogger('error', () => {});

let tmp: string;
let library: Library;
let core: Core;

// A client that keeps what it gets.
interface TestClient extends Client {
    messages: JsonObject[];
    // The messages named `name`, in order.
    of(name: string): JsonObject[];
    clear(): void;
}

function client(): TestClient {
    const messages: JsonObject[] = [];
    return {
        messages,
        send: m => messages.push(JSON.parse(JSON.stringify(m))),
        of: name => messages.filter(m => name in m).map(m => m[name] as JsonObject),
        clear: () => messages.splice(0),
    };
}

async function start(files: Record<string, JsonObject | string> = {lab: {carrier: 'colibri_8',
    modules: [{slot: 1, type: 'colibri_aiv'}, {slot: 3, type: 'colibri_ssr'}]}}, run = 'lab') {
    for (const [name, content] of Object.entries(files))
        writeFileSync(library.file(name), typeof content === 'string' ? content : JSON.stringify(content));
    core = new Core(catalog, library, new LinkServer(path.join(tmp, 'link.sock'), quiet), () => performance.now(),
        () => 0.5, quiet);
    await core.start(run);
    const a = client(), b = client();
    core.connect(a);
    core.connect(b);
    return {a, b};
}

// Sends a request from `c` and returns its answer.
async function ask(c: TestClient, request: object | string): Promise<JsonObject> {
    const before = c.messages.length;
    await core.receive(c, typeof request === 'string' ? request : JSON.stringify(request));
    const answers = c.messages.slice(before).filter(m => 'ok' in m || 'error' in m);
    expect(answers).toHaveLength(1);
    return answers[0]!;
}

const ok = {ok: {}};
const values = (c: TestClient) => Object.assign({}, ...c.of('state').map(s => s.values as JsonObject));

beforeEach(async () => {
    tmp = mkdtempSync(path.join(os.tmpdir(), 'colibri-core-'));
    library = new Library(tmp);
    await library.init();
});

afterEach(() => {
    core?.stop();
    rmSync(tmp, {recursive: true, force: true});
});

describe('a client', () => {
    it('gets the whole state on connect, in order', async () => {
        const {a} = await start();
        expect(a.messages.map(m => Object.keys(m)[0])).toEqual(
            ['hello', 'library', 'settings', 'scene', 'state', 'stats', 'link']);
        expect(a.of('hello')[0]).toEqual({protocol: 1, emulator: 'colibri', version: VERSION, features: ['copy']});
        expect(a.of('library')[0]).toEqual({environments: [{name: 'lab'}], running: 'lab', unsaved: false});
        expect(a.of('link')[0]).toEqual({state: 'waiting'});
        const scene = a.of('scene')[0]!;
        expect((scene.parts as JsonObject[]).map(p => [p.id, p.place, p.type])).toEqual([
            ['slot0', 'slot-0', 'carrier'], ['slot1', 'slot-1', 'colibri_aiv'], ['slot2', 'slot-2', null],
            ['slot3', 'slot-3', 'colibri_ssr'], ['slot4', 'slot-4', null], ['slot5', 'slot-5', null],
            ['slot6', 'slot-6', null], ['slot7', 'slot-7', null]]);
        const rows = scene.rows as JsonObject[];
        expect(rows.find(r => r.key === 'slot1.ai.1')).toMatchObject({part: 'slot1', label: 'slot1-ai[1]',
            kind: 'input', type: 'number', unit: 'V', control: 'slider'});
        expect(rows.find(r => r.key === 'slot3.dq.1')).toMatchObject({kind: 'output', type: 'bool', indicator: 'lamp'});
        const state = a.of('state')[0]!.values as JsonObject;
        expect(state).toMatchObject({'slot1.ai.1.source': 0, 'slot1.ai.1.noise': expect.any(Number), 'slot3.dq.1': false});
        expect(a.of('stats')[0]!.channels).toMatchObject({'slot3.dq.1': {received: 0, queued: 0, dropped: 0}});
    });

    it('sees the render copies: the board, each part in its place, each channel at its point', async () => {
        const {a} = await start();
        const scene = a.of('scene')[0]!;
        const carrier = catalog.carriers.get('colibri_8')!.def;
        expect(scene.board).toEqual({asset: 'assets/carriers/colibri_8/picture.webp', extras: []});
        const parts = scene.parts as JsonObject[];
        const place = carrier.places!.slots.find(r => r.slot === 1)!;
        expect(parts[1]).toMatchObject({id: 'slot1', asset: 'assets/modules/colibri_aiv/picture.webp',
            rect: {x: place.x, y: place.y, w: place.w, h: place.h}});
        expect(parts[2]).toHaveProperty('rect');                // empty: its place, no picture
        expect(parts[2]).not.toHaveProperty('asset');
        expect(parts[0]).not.toHaveProperty('rect');            // built in: on the board itself
        const aiv = catalog.modules.get('colibri_aiv')!.def.positions.find(p => p.index === 1)!;
        expect((scene.rows as JsonObject[]).find(r => r.key === 'slot1.ai.1')!.at)
            .toEqual({x: place.x + aiv.x * place.w, y: place.y + aiv.y * place.h});
        expect((scene.rows as JsonObject[]).find(r => r.key === 'slot3.dq.1')!.at).toBeDefined();
    });

    it('with a DII sees signal and measured rows', async () => {
        const {a} = await start({lab: {carrier: 'colibri_8', modules: [{slot: 2, type: 'colibri_dii'}]}});
        const rows = a.of('scene')[0]!.rows as JsonObject[];
        expect(rows.filter(r => r.part === 'slot2').map(r => [r.key, r.kind])).toEqual([
            ['slot2.in.1', 'signal'], ['slot2.in.2', 'signal'],
            ['slot2.di.1', 'measured'], ['slot2.di.2', 'measured'], ['slot2.freq.1', 'measured'],
            ['slot2.freq.2', 'measured'], ['slot2.duty.1', 'measured'], ['slot2.duty.2', 'measured'],
            ['slot2.count.1', 'measured'], ['slot2.count.2', 'measured']]);
        expect(rows[0]).toMatchObject({type: 'pulse', sources: ['pulse']});
    });

    it('gets an error for a message that is no request, with the id and the request named', async () => {
        const {a} = await start();
        expect(await ask(a, 'nonsense')).toEqual({error: {message: 'Invalid message'}});
        expect(await ask(a, '{"a": {}, "b": {}}')).toEqual({error: {message: 'Invalid message'}});
        expect(await ask(a, {set: 5})).toEqual({error: {request: 'set', message: 'Invalid message'}});
        expect(await ask(a, {fly: {id: 'x'}})).toEqual({error: {id: 'x', request: 'fly', message: 'Unknown request: fly'}});
        // copy is a feature (spec §5.3): its fields are checked first.
        expect(await ask(a, {copy: {id: 1}}))
            .toEqual({error: {id: 1, request: 'copy', message: 'Invalid field: address'}});
    });
});

describe('set', () => {
    it('broadcasts the new state to every client, then answers the sender, and makes it unsaved', async () => {
        const {a, b} = await start();
        a.clear();
        b.clear();
        expect(await ask(a, {set: {id: 3, key: 'slot1.ai.1.source', value: {sine: {offset: 5, amplitude: 1, period: 10}}}}))
            .toEqual({ok: {id: 3}});
        for (const c of [a, b]) {
            expect(values(c)['slot1.ai.1.source']).toEqual({sine: {offset: 5, amplitude: 1, period: 10}});
            expect(c.of('library').at(-1)!.unsaved).toBe(true);
        }
        expect(a.messages.findIndex(m => 'state' in m)).toBeLessThan(a.messages.findIndex(m => 'ok' in m));
        expect(b.of('ok')).toEqual([]);
    });

    it('sets several keys all or none', async () => {
        const {a} = await start();
        expect(await ask(a, {set: {values: {'slot1.ai.1.source': 4, 'slot1.ai.2.noise': 0}}})).toEqual(ok);
        expect(values(a)).toMatchObject({'slot1.ai.1.source': 4, 'slot1.ai.2.noise': 0});
        expect(await ask(a, {set: {values: {'slot1.ai.1.source': 6, 'slot9.ai.1.source': 1}}}))
            .toEqual({error: {request: 'set', message: 'Unknown key: slot9.ai.1.source'}});
        expect(values(a)['slot1.ai.1.source']).toBe(4);
    });

    it('refuses what the model refuses, and a request without its fields', async () => {
        const {a} = await start();
        const refused = async (body: object) => ((await ask(a, {set: body})).error as JsonObject).message;
        expect(await refused({key: 'slot1.ai.1', value: 3})).toBe('Not settable: slot1.ai.1');
        expect(await refused({key: 'slot3.dq.1.source', value: true})).toBe('Not settable: slot3.dq.1.source');
        expect(await refused({key: 'slot1.ai.1.source', value: {pulse: {frequency: 1, duty: 50}}}))
            .toBe('Source not accepted: slot1.ai.1.source');
        expect(await refused({key: 'slot1.ai.1.noise', value: -1})).toBe('Invalid noise: slot1.ai.1.noise');
        expect(await refused({value: 1})).toBe('Invalid field: key');
        expect(await refused({key: 'slot1.ai.1.source'})).toBe('Invalid field: value');
        expect(await refused({values: 3})).toBe('Invalid field: values');
    });

    it('sends the values the scan cycle computes, at most every 50 ms', async () => {
        const {a} = await start();
        await ask(a, {set: {key: 'slot1.ai.1.source', value: 7}});
        a.clear();
        await new Promise(r => setTimeout(r, 250));
        expect(values(a)['slot1.ai.1']).toBeCloseTo(7, 1);
        const states = a.of('state');
        expect(states.length).toBeGreaterThan(0);
        expect(states.length).toBeLessThanOrEqual(6);
    });
});

describe('configure', () => {
    it('changes the cycle time and the queue size at once, without a power cycle', async () => {
        const {a, b} = await start();
        a.clear();
        expect(await ask(a, {configure: {values: {cycleMs: 20, queueSize: 3}}})).toEqual(ok);
        expect(b.of('settings').at(-1)!.values).toMatchObject({cycleMs: 20, queueSize: 3});
        expect(a.of('scene')).toEqual([]);
        expect(a.of('library').at(-1)!.unsaved).toBe(true);
    });

    it('puts a module into a slot with its serial in one request: a power cycle that keeps the sources', async () => {
        const {a} = await start();
        await ask(a, {set: {key: 'slot1.ai.1.source', value: 7}});
        a.clear();
        expect(await ask(a, {configure: {values: {'slot4.serial': 'S-1', 'slot4.type': 'colibri_ssr'}}})).toEqual(ok);
        expect(a.messages.map(m => Object.keys(m)[0]).slice(0, 5)).toEqual(['settings', 'scene', 'state', 'stats', 'link']);
        const parts = a.of('scene')[0]!.parts as JsonObject[];
        expect(parts[4]).toMatchObject({id: 'slot4', type: 'colibri_ssr', name: 'Colibri SSR'});
        expect(values(a)).toMatchObject({'slot1.ai.1.source': 7, 'slot4.dq.1': false});
        expect(a.of('settings').at(-1)!.values).toMatchObject({'slot4.type': 'colibri_ssr', 'slot4.serial': 'S-1'});
        // Emptying the slot takes its serial field with it.
        await ask(a, {configure: {values: {'slot4.type': ''}}});
        expect(a.of('settings').at(-1)!.values).not.toHaveProperty('slot4.serial');
    });

    it('refuses an unknown or read-only setting and an invalid value, changing nothing', async () => {
        const {a} = await start();
        const refused = async (v: object) => ((await ask(a, {configure: {values: v}})).error as JsonObject).message;
        expect(await refused({speed: 1})).toBe('Unknown setting: speed');
        expect(await refused({'slot0.type': 'colibri_aiv'})).toBe('Read-only: slot0.type');
        expect(await refused({cycleMs: 0})).toBe('Invalid value: cycleMs');
        expect(await refused({cycleMs: 2.5})).toBe('Invalid value: cycleMs');
        expect(await refused({'slot4.type': 'colibri_flux'})).toBe('Invalid value: slot4.type');
        expect(await refused({'slot4.type': 'carrier'})).toBe('Invalid value: slot4.type');
        expect(await refused({cycleMs: 20, 'slot9.type': 'colibri_ssr'})).toBe('Unknown setting: slot9.type');
        expect(a.of('settings').at(-1)!.values).toMatchObject({cycleMs: 10});
        expect(a.of('library').at(-1)!.unsaved).toBe(false);
        // The same value again changes nothing.
        expect(await ask(a, {configure: {values: {'slot0.type': 'carrier', cycleMs: 10}}})).toEqual(ok);
        expect(a.of('library').at(-1)!.unsaved).toBe(false);
    });

    it('gives a communication module its type\'s fields; its parameters are no power cycle', async () => {
        const {a} = await start();
        await ask(a, {configure: {values: {'comm1.type': 'colibri_wifi', 'comm1.ip.mode': 'dhcp'}}});
        const fields = (a.of('settings').at(-1)!.fields as JsonObject[]).filter(f => f.group === 'Communication');
        expect(fields.map(f => f.id)).toEqual(['comm1.type', 'comm1.ip.mode', 'comm1.ip.address', 'comm1.ip.prefix',
            'comm1.ip.gateway', 'comm1.ip.dns', 'comm1.ip.mac', 'comm1.wifi.ssid']);
        expect(fields[0]).toMatchObject({restart: true});
        a.clear();
        expect(await ask(a, {configure: {values: {'comm1.wifi.ssid': 'plant', 'comm1.ip.dns': ['9.9.9.9']}}})).toEqual(ok);
        expect(a.of('scene')).toEqual([]);
        expect(a.of('settings').at(-1)!.values).toMatchObject({'comm1.wifi.ssid': 'plant', 'comm1.ip.dns': ['9.9.9.9'],
            'comm1.ip.address': null});
        // To Ethernet: the address is kept, the SSID goes.
        await ask(a, {configure: {values: {'comm1.type': 'colibri_ethernet'}}});
        const v = a.of('settings').at(-1)!.values as JsonObject;
        expect(v['comm1.ip.mode']).toBe('dhcp');
        expect(v).not.toHaveProperty('comm1.wifi.ssid');
        // LoRaWAN's EUIs are 16 hex digits.
        await ask(a, {configure: {values: {'comm1.type': 'colibri_lorawan'}}});
        expect(((await ask(a, {configure: {values: {'comm1.lorawan.devEui': 'xyz'}}})).error as JsonObject).message)
            .toBe('Invalid value: comm1.lorawan.devEui');
        expect(await ask(a, {configure: {values: {'comm1.lorawan.devEui': '0011223344556677'}}})).toEqual(ok);
    });
});

describe('persist', () => {
    it('writes the whole running environment, and it is saved', async () => {
        const {a, b} = await start();
        await ask(a, {set: {key: 'slot1.ai.1.source', value: 7}});
        await ask(a, {configure: {values: {cycleMs: 20, 'comm1.type': 'colibri_ethernet', 'comm1.ip.mode': 'static'}}});
        expect(await ask(a, {persist: {}})).toEqual(ok);
        expect(b.of('library').at(-1)!.unsaved).toBe(false);
        const file = JSON.parse(readFileSync(library.file('lab'), 'utf8'));
        expect(file).toMatchObject({carrier: 'colibri_8', cycleMs: 20,
            comm: [{slot: 1, type: 'colibri_ethernet', ip: {mode: 'static'}}],
            modules: [{slot: 1, type: 'colibri_aiv'}, {slot: 3, type: 'colibri_ssr'}],
            inputs: {'slot1.ai.1': {source: 7}}});
    });

    it('fails with the write\'s error and a notice to every client', async () => {
        const {a, b} = await start();
        rmSync(library.environments, {recursive: true});
        const answer = await ask(a, {persist: {}});
        expect((answer.error as JsonObject).message).toMatch(/ENOENT/);
        expect(b.of('notice').at(-1)).toMatchObject({level: 'error', message: expect.stringMatching(/^Persist failed: /)});
    });
});

describe('the library', () => {
    it('creates, copies, renames and deletes environments', async () => {
        const {a, b} = await start();
        expect(await ask(a, {create: {name: 'lab2'}})).toEqual(ok);
        expect(await ask(a, {create: {name: 'lab3', from: 'lab'}})).toEqual(ok);
        expect(JSON.parse(readFileSync(library.file('lab3'), 'utf8')).modules).toHaveLength(2);
        expect(await ask(a, {rename: {name: 'lab2', to: 'spare'}})).toEqual(ok);
        expect(await ask(a, {delete: {name: 'lab3'}})).toEqual(ok);
        expect(b.of('library').at(-1)).toEqual({environments: [{name: 'lab'}, {name: 'spare'}], running: 'lab', unsaved: false});
        const refused = async (r: object) => ((await ask(a, r)).error as JsonObject).message;
        expect(await refused({create: {name: '.x'}})).toBe('Invalid name');
        expect(await refused({create: {name: 'spare'}})).toBe('Environment exists: spare');
        expect(await refused({create: {name: 'y', from: 'nope'}})).toBe('Unknown environment: nope');
        expect(await refused({delete: {name: 'lab'}})).toBe('Environment running: lab');
        expect(await refused({rename: {name: 'nope', to: 'z'}})).toBe('Unknown environment: nope');
        expect(await refused({rename: {name: 'lab'}})).toBe('Invalid field: to');
    });

    it('renames the running environment: a power cycle under the new name', async () => {
        const {a} = await start();
        await ask(a, {set: {key: 'slot1.ai.1.source', value: 7}});
        a.clear();
        expect(await ask(a, {rename: {name: 'lab', to: 'bench'}})).toEqual(ok);
        expect(a.of('scene')).toHaveLength(1);
        expect(a.of('library').at(-1)).toEqual({environments: [{name: 'bench'}], running: 'bench', unsaved: true});
        expect(values(a)['slot1.ai.1.source']).toBe(7);
        expect(core.running).toBe('bench');
    });

    it('selects another environment, asking first when there are unsaved changes', async () => {
        const {a} = await start({lab: {carrier: 'colibri_8'}, pump: {carrier: 'colibri_8',
            modules: [{slot: 5, type: 'colibri_ssr'}]}, broken: '{"carrier": "colibri_99"}'});
        await ask(a, {configure: {values: {cycleMs: 30}}});
        const refused = async (r: object) => ((await ask(a, r)).error as JsonObject).message;
        expect(await refused({select: {name: 'pump'}})).toBe('Unsaved changes');
        expect(await ask(a, {select: {name: 'pump', discard: true}})).toEqual(ok);
        expect(a.of('library').at(-1)).toMatchObject({running: 'pump', unsaved: false});
        expect((a.of('scene').at(-1)!.parts as JsonObject[])[5]).toMatchObject({type: 'colibri_ssr'});
        expect(await library.lastRun()).toBe('pump');
        expect(await refused({select: {name: 'broken'}})).toBe('Cannot be run: broken: unknown carrier type colibri_99');
        expect(await refused({select: {name: 'nope'}})).toBe('Unknown environment: nope');
        expect(core.running).toBe('pump');
    });

    it('runs nothing when the startup environment cannot be run, until one is selected', async () => {
        const {a} = await start({broken: '[', lab: {carrier: 'colibri_8'}}, 'broken');
        expect(a.of('library')[0]).toMatchObject({running: null});
        expect(a.of('settings')[0]).toEqual({fields: [], values: {}});
        expect(a.of('scene')[0]).toEqual({board: null, parts: [], rows: []});
        expect(((await ask(a, {set: {key: 'slot1.ai.1.source', value: 1}})).error as JsonObject).message).toBe('No environment');
        expect(((await ask(a, {persist: {}})).error as JsonObject).message).toBe('No environment');
        expect(await ask(a, {select: {name: 'lab'}})).toEqual(ok);
        expect(a.of('library').at(-1)).toMatchObject({running: 'lab'});
    });
});

describe('resetstats', () => {
    it('sets every counter to 0 and broadcasts them all', async () => {
        const {a, b} = await start();
        b.clear();
        expect(await ask(a, {resetstats: {}})).toEqual(ok);
        const stats = b.of('stats')[0]!;
        expect(Object.keys(stats.channels as object)).toContain('slot1.ai.1');
        expect((stats.channels as JsonObject)['slot1.ai.1']).toEqual({sent: 0});
    });
});

describe('the MCU module', () => {
    async function served(carrier: string, mcu: string) {
        writeFileSync(library.file('lab'), JSON.stringify({carrier}));
        const lines: string[] = [];
        const log = createLogger('warn', l => lines.push(l));
        const link = new LinkServer(path.join(tmp, 'link.sock'), quiet);
        const serve = vi.spyOn(link, 'serve');
        core = new Core(catalog, library, link, () => performance.now(), () => 0.5, log, catalog.mcu.get(mcu)!.def);
        await core.start('lab');
        return {description: serve.mock.calls.at(-1)![0]!.description, lines};
    }

    it('is described when the carrier takes it', async () => {
        const {description, lines} = await served('colibri_8', 'colibri_mcu2');
        expect(description.carrier.mcu).toMatchObject({type: 'colibri_mcu2', name: 'Colibri MCU2',
            vendor: 'CurrentMakers'});
        expect(lines).toEqual([]);
    });

    it('is left out, with a warning, when the carrier does not take it', async () => {
        catalog.mcu.set('other_mcu', {def: {type: 'other_mcu'}, dir: tmp});
        try {
            const {description, lines} = await served('colibri_8', 'other_mcu');
            expect(description.carrier).not.toHaveProperty('mcu');
            expect(lines).toEqual(['warn environment lab: carrier colibri_8 takes no MCU module other_mcu: not described']);
        } finally {
            catalog.mcu.delete('other_mcu');
        }
    });
});
