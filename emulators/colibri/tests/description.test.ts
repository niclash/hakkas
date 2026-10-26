import {describe as group, expect, it} from 'vitest';
import {loadCatalog} from '../src/catalog.js';
import {Checker, type JsonObject} from '../src/check.js';
import {checkDescription, describe, type Description} from '../src/description.js';
import {readEnvironment} from '../src/environment.js';
import {createLogger} from '../src/log.js';
import {BUILTIN_DATA} from '../src/package.js';
import {System} from '../src/system.js';

const catalog = await loadCatalog([BUILTIN_DATA], createLogger('error', () => {}));

function described(env: JsonObject, name = 'lab', mcu?: string): Description {
    const c = new Checker();
    const environment = readEnvironment(c, {carrier: 'colibri_8', ...env});
    expect(c.errors).toEqual([]);
    const {system} = System.build(name, environment!, catalog, () => 0, () => 0.5);
    return describe(system, mcu === undefined ? undefined : catalog.mcu.get(mcu)!.def);
}

const modules: JsonObject[] = [
    {slot: 1, type: 'colibri_aiv', serial: 'EMU-0001'},
    {slot: 2, type: 'colibri_dii'},
    {slot: 3, type: 'colibri_ssr'},
    {slot: 4, type: 'colibri_dp1'},
    {slot: 6, type: 'colibri_flux'},    // not in the catalog: an empty slot
];

group('the environment description', () => {
    it('names the environment and its carrier', () => {
        const d = described({modules});
        expect([d.name, d.description, d.icon]).toEqual(['lab', 'Colibri emulator: Colibri 8', 'chip']);
    });

    it('has a port per slot and channel group, sensors and actuators apart, in slot order', () => {
        const d = described({modules});
        expect(d.outPorts.map(p => p.id)).toEqual([
            'slot1-ai', 'slot2-di', 'slot2-freq', 'slot2-duty', 'slot2-count', 'slot4-ai',
        ]);
        expect(d.inPorts.map(p => p.id)).toEqual(['slot3-dq']);
        const aiv = catalog.modules.get('colibri_aiv')!.def.channels[0]!;
        expect(d.outPorts[0]).toEqual({id: 'slot1-ai', type: 'number', description: aiv.description,
            unit: 'V', min: aiv.min, max: aiv.max, addressable: true, count: aiv.count});
        expect(d.outPorts[1]).toMatchObject({id: 'slot2-di', type: 'bool', addressable: true, count: 2});
        expect(d.outPorts[1]).not.toHaveProperty('unit');
        // A group of one channel is a plain port.
        expect(d.outPorts[5]).not.toHaveProperty('addressable');
        expect(d.outPorts[5]).not.toHaveProperty('count');
    });

    it('lists the filled slots\' modules, the built-in one marked, with their ports', () => {
        const d = described({modules});
        expect(d.modules.map(m => [m.slot, m.type, m.builtin ?? false])).toEqual([
            [0, 'carrier', true], [1, 'colibri_aiv', false], [2, 'colibri_dii', false],
            [3, 'colibri_ssr', false], [4, 'colibri_dp1', false],
        ]);
        expect(d.modules[0]).toEqual({slot: 0, builtin: true, type: 'carrier', ports: []});
        expect(d.modules[1]).toMatchObject({name: 'Colibri AIV', vendor: 'CurrentMakers', serial: 'EMU-0001',
            ports: ['slot1-ai']});
        expect(d.modules[2]!.ports).toEqual(['slot2-di', 'slot2-freq', 'slot2-duty', 'slot2-count']);
        expect(d.modules[2]).not.toHaveProperty('serial');
    });

    it('describes the carrier with its power and communication slots, parameters as given', () => {
        const ip = {mode: 'dhcp', address: '192.168.1.40', prefix: 24};
        const d = described({modules, comm: [{slot: 1, type: 'colibri_ethernet', ip}]});
        const def = catalog.carriers.get('colibri_8')!.def;
        expect(d.carrier).toEqual({
            type: 'colibri_8', name: 'Colibri 8', vendor: 'CurrentMakers',
            documentation: 'https://currentmakers.com/products/colibri/colibri_8/',
            picture: def.render,
            slots: 7, builtin: [{slot: 0}],
            power: {voltage: {min: 7, max: 30}, ac: true, dc: true, current: 2.0},
            comm: [{slot: 1, type: 'colibri_ethernet', name: 'Colibri Ethernet', ip}],
            places: {slots: def.places!.slots, comm: def.places!.comm},
        });
        expect(described({}).carrier.comm).toEqual([]);
    });

    it('names the MCU module, if one is given', () => {
        const d = described({}, 'lab', 'colibri_mcu6');
        expect(d.carrier.mcu).toEqual({type: 'colibri_mcu6', name: 'Colibri MCU6', vendor: 'CurrentMakers',
            picture: catalog.mcu.get('colibri_mcu6')!.def.render});
        expect((d.carrier.mcu as JsonObject).picture).toMatchObject({rotation: 90});
        // Its slot's place only with a module in it.
        expect((d.carrier.places as JsonObject).mcu).toEqual(catalog.carriers.get('colibri_8')!.def.places!.mcu);
        expect(described({}).carrier).not.toHaveProperty('mcu');
        expect(described({}).carrier.places).not.toHaveProperty('mcu');
    });

    it('gives each module its picture link and its channels\' positions, under their port ids', () => {
        const d = described({modules: [{slot: 2, type: 'colibri_aiv'}, {slot: 3, type: 'colibri_dp1'},
            {slot: 4, type: 'colibri_rs485i'}]});
        const aiv = catalog.modules.get('colibri_aiv')!.def;
        expect(d.modules[1]!.picture).toEqual(aiv.render);
        expect(d.modules[1]!.picture).toHaveProperty('crop');
        expect(d.modules[1]!.positions).toEqual(aiv.positions.map(p => ({port: 'slot2-ai', index: p.index,
            x: p.x, y: p.y})));
        // A group of one channel: no index.
        expect(d.modules[2]!.positions![0]).toEqual({port: 'slot3-ai', x: expect.any(Number), y: expect.any(Number)});
        // No picture (RS485I has no published render), no positions: neither member.
        expect(d.modules[3]).not.toHaveProperty('picture');
        expect(d.modules[3]).not.toHaveProperty('positions');
    });

    it('passes hakkas\' checks with every module type in the catalog', () => {
        const types = [...catalog.modules.keys()].filter(t => t !== 'carrier');
        for (let i = 0; i < types.length; i += 7) {
            const d = described({modules: types.slice(i, i + 7).map((type, k) => ({slot: k + 1, type}))});
            expect(checkDescription(d)).toEqual([]);
        }
    });
});

group('hakkas\' checks of a description (04 §0)', () => {
    const ok = described({modules});
    const port = (id: string) => ({id, type: 'number' as const});

    it('find nothing wrong with a good one', () => {
        expect(checkDescription(ok)).toEqual([]);
    });

    it('limit the name, the ports, the modules and the texts', () => {
        expect(checkDescription({...ok, name: 'x'.repeat(65)})).toEqual(['name: expected 1 to 64 bytes']);
        expect(checkDescription({...ok, name: 'é'.repeat(33)})).toEqual(['name: expected 1 to 64 bytes']);
        expect(checkDescription({...ok, outPorts: Array.from({length: 257}, (_, i) => port(`p${i}`)), inPorts: []}))
            .toEqual(['257 ports, more than 256']);
        expect(checkDescription({...ok, inPorts: [port('slot1-ai')]})).toEqual(['duplicate port id "slot1-ai"']);
        expect(checkDescription({...ok, inPorts: [port('x'.repeat(65))]}))
            .toEqual([`${'x'.repeat(65)}: id: expected 1 to 64 bytes`]);
        expect(checkDescription({...ok, inPorts: [{...port('q'), addressable: true, count: 1025}]}))
            .toEqual(['q: count: expected 1 to 1024']);
        expect(checkDescription({...ok, inPorts: [{...port('q'), min: 2, max: 1}]}))
            .toEqual(['q: min greater than max']);
        expect(checkDescription({...ok, inPorts: [{...port('q'), unit: 'u'.repeat(1025)}]}))
            .toEqual(['q: text longer than 1024 bytes']);
        const placed = (n: number) => ({slot: 1, type: 't', ports: [],
            positions: Array.from({length: n}, () => ({port: 'p', x: 0, y: 0}))});
        expect(checkDescription({...ok, modules: [placed(1024)]})).toEqual([]);
        expect(checkDescription({...ok, modules: [placed(1000), {...placed(25), slot: 2}]}))
            .toEqual(['1025 positions, more than 1024']);
        expect(checkDescription({...ok, modules: Array.from({length: 65}, (_, i) => ({slot: i, type: 't', ports: []}))}))
            .toEqual(['65 modules, more than 64']);
        expect(checkDescription({...ok, modules: [{slot: 1, type: 't', serial: 's'.repeat(1025), ports: []}]}))
            .toEqual(['slot 1: text longer than 1024 bytes']);
    });
});
