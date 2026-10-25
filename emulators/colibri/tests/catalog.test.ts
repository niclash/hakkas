import {mkdirSync, mkdtempSync, rmSync, writeFileSync} from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {afterEach, beforeEach, describe, expect, it} from 'vitest';
import {loadCatalog, type Catalog} from '../src/catalog.js';
import {isMeasured, type Channel} from '../src/descriptors.js';
import {createLogger} from '../src/log.js';
import {BUILTIN_DATA} from '../src/package.js';

async function load(dirs: string[]): Promise<{catalog: Catalog, lines: string[]}> {
    const lines: string[] = [];
    const catalog = await loadCatalog(dirs, createLogger('fine', l => lines.push(l)));
    return {catalog, lines};
}

// The channels as the spec's §2.1 table lists them: id, direction, type, count, unit.
function table(channels: Channel[]): string[] {
    return channels.map(c => [c.id, c.direction, c.type, c.count, c.unit ?? '', isMeasured(c) ? 'measured' : '']
        .join(' ').trim());
}

describe('the built-in catalog', () => {
    it('loads without a warning', async () => {
        const {catalog, lines} = await load([BUILTIN_DATA]);
        expect(lines).toEqual(['info catalog: module types 18, carrier types 1, communication module types 4, ' +
            'MCU module types 3']);
        expect([...catalog.modules.keys()].sort()).toEqual(['carrier', 'colibri_aic', 'colibri_aiv', 'colibri_aqv',
            'colibri_dii', 'colibri_dio1', 'colibri_diu', 'colibri_dp1', 'colibri_dp2', 'colibri_dp3',
            'colibri_fallback', 'colibri_fet', 'colibri_pid1', 'colibri_pt1000', 'colibri_rs485i', 'colibri_rs485u',
            'colibri_ssr', 'colibri_triac1']);
        expect([...catalog.comm.keys()].sort())
            .toEqual(['colibri_ethernet', 'colibri_lorawan', 'colibri_rs485', 'colibri_wifi']);
        expect([...catalog.mcu.keys()].sort()).toEqual(['colibri_mcu2', 'colibri_mcu6', 'colibri_mcu7']);
    });

    it('names every type "Colibri …", except the carrier’s built-in module', async () => {
        const {catalog} = await load([BUILTIN_DATA]);
        for (const map of [catalog.modules, catalog.carriers, catalog.comm, catalog.mcu])
            for (const [type, {def}] of map)
                expect(def.name, type).toEqual(type === 'carrier' ? undefined : expect.stringMatching(/^Colibri /));
        expect(catalog.modules.get('colibri_pt1000')!.def.name).toBe('Colibri Pt1000');
    });

    it('has the Colibri 8 carrier', async () => {
        const {catalog} = await load([BUILTIN_DATA]);
        const c = catalog.carriers.get('colibri_8')!.def;
        expect(c).toMatchObject({
            name: 'Colibri 8', slots: 7, builtin: [{slot: 0, type: 'carrier'}],
            power: {voltage: {min: 7, max: 30}, ac: true, dc: true, current: 2},
            comm: [{slot: 1, types: ['colibri_ethernet', 'colibri_wifi', 'colibri_rs485', 'colibri_lorawan']}],
            mcu: {types: ['colibri_mcu2', 'colibri_mcu6', 'colibri_mcu7']},
        });
    });

    it('has the channels of the spec’s table', async () => {
        const {catalog} = await load([BUILTIN_DATA]);
        const channels = (type: string) => table(catalog.modules.get(type)!.def.channels);
        expect(channels('colibri_aiv')).toEqual(['ai input number 2 V']);
        expect(channels('colibri_aic')).toEqual(['ai input number 2 mA']);
        expect(channels('colibri_aqv')).toEqual(['aq output number 2 V']);
        expect(channels('colibri_pt1000')).toEqual(['ai input number 2 °C', 'fault input bool 2']);
        for (const dp of ['colibri_dp1', 'colibri_dp2', 'colibri_dp3'])
            expect(channels(dp)).toEqual(['ai input number 1 Pa']);
        expect(channels('colibri_dii')).toEqual(['di input bool 2  measured', 'freq input number 2 Hz measured',
            'duty input number 2 % measured', 'count input number 2  measured']);
        expect(channels('colibri_diu')).toEqual(['di input bool 4  measured', 'freq input number 4 Hz measured',
            'duty input number 4 % measured', 'count input number 4  measured']);
        expect(catalog.modules.get('colibri_diu')!.def.signals.map(s => [s.id, s.count])).toEqual([['in', 4]]);
        expect(channels('colibri_dio1'))
            .toEqual(['dir output bool 4', 'dq output bool 4', 'di input bool 4', 'fault input bool 4']);
        expect(channels('colibri_fet')).toEqual(['dq output bool 4']);
        expect(channels('colibri_ssr')).toEqual(['dq output bool 2']);
        expect(channels('colibri_triac1')).toEqual(['dq output bool 4']);
        expect(channels('colibri_fallback')).toEqual(['dq output bool 1']);
        expect(channels('colibri_pid1')).toEqual(['ai input number 1 °C', 'output input number 1 V',
            'saturated input bool 1', 'setpoint output number 1 °C', 'kp output number 1', 'ki output number 1 1/s',
            'kd output number 1 s', 'max_dgain output number 1', 'min output number 1 V', 'max output number 1 V',
            'reverse output bool 1', 'auto output bool 1', 'manual output number 1 V', 'sample_ms output number 1 ms']);
        for (const none of ['colibri_rs485i', 'colibri_rs485u', 'carrier'])
            expect(channels(none)).toEqual([]);
    });

    it('counts the DII pulses in whole units up to 32 bits', async () => {
        const {catalog} = await load([BUILTIN_DATA]);
        expect(catalog.modules.get('colibri_dii')!.def.channels.find(c => c.id === 'count'))
            .toMatchObject({min: 0, max: 4294967295, resolution: 1});
    });
});

describe('loading plugin directories', () => {
    let tmp: string;
    beforeEach(() => {
        tmp = mkdtempSync(path.join(os.tmpdir(), 'colibri-catalog-'));
    });
    afterEach(() => rmSync(tmp, {recursive: true, force: true}));

    function file(rel: string, content: unknown): void {
        const p = path.join(tmp, rel);
        mkdirSync(path.dirname(p), {recursive: true});
        writeFileSync(p, typeof content === 'string' ? content : JSON.stringify(content));
    }

    it('lets a later directory replace a type, with a warning', async () => {
        file('a/modules/m/module.json', {type: 'm', name: 'First'});
        file('b/modules/m/module.json', {type: 'm', name: 'Second'});
        const {catalog, lines} = await load([path.join(tmp, 'a'), path.join(tmp, 'b')]);
        expect(catalog.modules.get('m')!.def.name).toBe('Second');
        expect(catalog.modules.get('m')!.dir).toBe(path.join(tmp, 'b', 'modules', 'm'));
        expect(lines[0]).toBe(`warn ${path.join(tmp, 'b/modules/m/module.json')}: replaces the module type m of ` +
            path.join(tmp, 'a/modules/m'));
    });

    it('skips descriptors that cannot be read or are invalid, naming the file and the problem', async () => {
        file('p/modules/good/module.json', {type: 'good'});
        file('p/modules/broken/module.json', '{"type": ');
        file('p/modules/wrong/module.json', {type: 'other'});
        mkdirSync(path.join(tmp, 'p/modules/empty'));
        file('p/modules/stray.json', {});
        const {catalog, lines} = await load([path.join(tmp, 'p')]);
        expect([...catalog.modules.keys()]).toEqual(['good']);
        const f = (name: string) => path.join(tmp, 'p/modules', name, 'module.json');
        expect(lines).toEqual([
            expect.stringMatching(new RegExp(`^error ${f('broken')}: .*JSON.*: skipped$`)),
            `error ${f('empty')}: missing: skipped`,
            `error ${f('wrong')}: type: differs from its directory's name, wrong`,
            `error ${f('wrong')}: skipped`,
            'info catalog: module types 1, carrier types 0, communication module types 0, MCU module types 0',
        ]);
    });

    it('warns about a directory without descriptors', async () => {
        const {lines} = await load([path.join(tmp, 'nothing')]);
        expect(lines[0]).toBe(`warn ${path.join(tmp, 'nothing')}: no modules, carriers, comm or mcu directory`);
    });

    it('leaves out a picture that is not there', async () => {
        file('p/modules/m/module.json', {type: 'm', faceplate: 'face.svg'});
        file('p/modules/n/module.json', {type: 'n', faceplate: 'face.svg'});
        file('p/modules/n/face.svg', '<svg/>');
        const {catalog, lines} = await load([path.join(tmp, 'p')]);
        expect(catalog.modules.get('m')!.def.faceplate).toBeUndefined();
        expect(catalog.modules.get('n')!.def.faceplate).toBe('face.svg');
        expect(lines[0]).toBe(`warn ${path.join(tmp, 'p/modules/m/module.json')}: faceplate face.svg not found, left out`);
    });

    it('checks what a carrier names against the catalog', async () => {
        file('p/modules/io/module.json', {type: 'io'});
        file('p/comm/eth/comm.json', {type: 'eth', params: ['ip']});
        file('p/carriers/ok/carrier.json', {type: 'ok', slots: 2, builtin: [{slot: 0, type: 'io'}],
            comm: [{slot: 1, types: ['eth', 'smoke_signals']}], mcu: {types: ['m1', 'm9']}});
        file('p/mcu/m1/mcu.json', {type: 'm1', name: 'M1'});
        file('p/carriers/bad/carrier.json', {type: 'bad', slots: 2, builtin: [{slot: 0, type: 'nope'}]});
        const {catalog, lines} = await load([path.join(tmp, 'p')]);
        expect([...catalog.carriers.keys()]).toEqual(['ok']);
        expect(catalog.carriers.get('ok')!.def.comm).toEqual([{slot: 1, types: ['eth']}]);
        expect(catalog.carriers.get('ok')!.def.mcu).toEqual({types: ['m1']});
        const f = (name: string) => path.join(tmp, 'p/carriers', name, 'carrier.json');
        expect(lines).toEqual([
            `error ${f('bad')}: built-in slot 0: unknown module type nope`,
            `error ${f('bad')}: skipped`,
            `warn ${f('ok')}: communication slot 1: unknown communication module type smoke_signals, left out`,
            `warn ${f('ok')}: unknown MCU module type m9, left out`,
            'info catalog: module types 1, carrier types 1, communication module types 1, MCU module types 1',
        ]);
    });
});
