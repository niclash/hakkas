import {describe, expect, it} from 'vitest';
import {loadCatalog, type Catalog} from '../src/catalog.js';
import {Checker, type Json, type JsonObject} from '../src/check.js';
import {checkEnvironment, newEnvironment, readEnvironment, writeEnvironment, type Environment} from '../src/environment.js';
import {createLogger} from '../src/log.js';
import {BUILTIN_DATA} from '../src/package.js';

function read(v: Json): {env?: Environment, errors: string[], warnings: string[]} {
    const c = new Checker();
    const env = readEnvironment(c, v);
    return {env, errors: c.errors, warnings: c.warnings};
}

function errors(v: JsonObject): string[] {
    const r = read({carrier: 'colibri_8', ...v});
    expect(r.env).toBeUndefined();
    return r.errors;
}

const catalog: Catalog = await loadCatalog([BUILTIN_DATA], createLogger('error', () => {}));

function check(v: JsonObject) {
    const r = read({carrier: 'colibri_8', ...v});
    expect(r.errors).toEqual([]);
    return checkEnvironment(r.env!, catalog);
}

// The spec's §5 example.
const example: JsonObject = {
    carrier: 'colibri_8', cycleMs: 10, queueSize: 10,
    comm: [{slot: 1, type: 'colibri_ethernet', ip: {mode: 'dhcp', address: '192.168.1.40', prefix: 24,
        gateway: '192.168.1.1', dns: ['192.168.1.1'], mac: '02:00:00:00:00:01'}}],
    modules: [{slot: 1, type: 'colibri_aiv', serial: 'EMU-0001'}, {slot: 3, type: 'colibri_ssr'}],
    inputs: {'slot1.ai.1': {source: 5.0, noise: 0.02}, 'slot2.in.1': {source: {pulse: {frequency: 10, duty: 50}}}},
};

describe('environment files', () => {
    it('read the spec’s example, and check it against the catalog', () => {
        const r = read(example);
        expect(r.errors).toEqual([]);
        expect(r.warnings).toEqual([]);
        expect(r.env).toEqual({...example, extra: {}});
        expect(checkEnvironment(r.env!, catalog)).toEqual({errors: [], warnings: []});
    });

    it('fill in the defaults', () => {
        expect(read({carrier: 'colibri_8'}).env).toEqual(newEnvironment());
    });

    it('write back what they read, unknown members included', () => {
        const text = writeEnvironment(read({...example, note: 'lab bench', zz: [1]}).env!);
        expect(JSON.parse(text)).toEqual({...example, note: 'lab bench', zz: [1]});
        expect(Object.keys(JSON.parse(text))).toEqual(
            ['carrier', 'cycleMs', 'queueSize', 'comm', 'modules', 'inputs', 'note', 'zz']);
        expect(text.endsWith('}\n')).toBe(true);
    });

    it('take a whole carrier definition, as a copy from a device has it', () => {
        const carrier = {type: 'acme_board', name: 'Acme', slots: 3, builtin: [{slot: 0, type: 'carrier'}], comm: []};
        const r = read({carrier});
        expect(r.errors).toEqual([]);
        expect(r.env!.carrier).toEqual(carrier);
        expect(JSON.parse(writeEnvironment(r.env!)).carrier).toEqual(carrier);
        expect(checkEnvironment(r.env!, catalog)).toEqual({errors: [], warnings: []});
        expect(errors({carrier: {type: 'x', slots: 99}}))
            .toEqual(['carrier: slots: expected an integer from 0 to 31', 'carrier: slots: expected 1 to 31 I/O slots, built-in ones included']);
    });

    it('refuse a malformed form', () => {
        expect(read([]).errors).toEqual(['expected an object']);
        expect(read({}).errors).toEqual(['carrier: missing']);
        expect(errors({carrier: 5})).toEqual(['carrier: expected a carrier type or a carrier definition']);
        expect(errors({cycleMs: 0})).toEqual(['cycleMs: expected an integer from 1 to 1000']);
        expect(errors({queueSize: 1001})).toEqual(['queueSize: expected an integer from 1 to 1000']);
        expect(errors({modules: [{slot: 1}]})).toEqual(['modules[0].type: missing']);
        expect(errors({modules: [{slot: 1, type: 'Colibri AIV'}]})).toEqual(['modules[0].type: expected a type name']);
        expect(errors({modules: [{slot: 31, type: 'x'}]})).toEqual(['modules[0].slot: expected an integer from 0 to 30']);
        expect(errors({modules: [{slot: 1, type: 'a'}, {slot: 1, type: 'b'}]})).toEqual(['modules[1].slot: slot 1 is listed twice']);
        expect(errors({comm: [{slot: 0, type: 'x'}]})).toEqual(['comm[0].slot: expected an integer from 1 to 4']);
        expect(errors({comm: [{slot: 1, type: 'x'}, {slot: 1, type: 'y'}]}))
            .toEqual(['comm[1].slot: communication slot 1 is listed twice']);
        expect(errors({comm: [{slot: 1, type: 'x', ip: {mode: 'auto', dns: '1.1.1.1'}}]}))
            .toEqual(['comm[0].ip.mode: expected one of dhcp, static', 'comm[0].ip.dns: expected a list of addresses']);
        expect(errors({comm: [{slot: 1, type: 'x', rs485: {parity: 'mark'}}]}))
            .toEqual(['comm[0].rs485.parity: expected one of none, even, odd']);
        expect(errors({comm: [{slot: 1, type: 'x', lorawan: {appKeySet: 'yes'}}]}))
            .toEqual(['comm[0].lorawan.appKeySet: expected true or false']);
        expect(errors({inputs: []})).toEqual(['inputs: expected an object']);
        expect(errors({inputs: {'slot1.ai.1': 5}})).toEqual(['inputs.slot1.ai.1: expected an object']);
    });

    it('warn about unknown members inside the known ones', () => {
        const r = read({carrier: 'colibri_8', modules: [{slot: 1, type: 'x', colour: 'red'}],
            comm: [{slot: 1, type: 'x', wifi: {ssid: 'lab', passphrase: 'secret'}}]});
        expect(r.errors).toEqual([]);
        expect(r.warnings).toEqual(['modules[0].colour: unknown member, ignored',
            'comm[0].wifi.passphrase: unknown member, ignored']);
    });

    it('cannot be run with what the carrier does not have', () => {
        const unknown = read({carrier: 'colibri_99'}).env!;
        expect(checkEnvironment(unknown, catalog)).toEqual({errors: ['unknown carrier type colibri_99'], warnings: []});
        expect(check({modules: [{slot: 8, type: 'colibri_aiv'}]}).errors).toEqual(['slot 8: the carrier has slots 0 to 7']);
        expect(check({modules: [{slot: 0, type: 'colibri_aiv'}]}).errors)
            .toEqual(['slot 0: a built-in slot, its module cannot be changed']);
        expect(check({comm: [{slot: 2, type: 'colibri_wifi'}]}).errors)
            .toEqual(['communication slot 2: the carrier has no such slot']);
    });

    it('leave slots empty, with a warning, for types the catalog lacks or the slot does not take', () => {
        expect(check({modules: [{slot: 2, type: 'colibri_flux'}]}))
            .toEqual({errors: [], warnings: ['slot 2: unknown module type colibri_flux, the slot stays empty']});
        expect(check({comm: [{slot: 1, type: 'colibri_zigbee'}]}).warnings)
            .toEqual(['communication slot 1: unknown communication module type colibri_zigbee, the slot stays empty']);
        expect(check({comm: [{slot: 1, type: 'colibri_ethernet', rs485: {bitrate: 9600}}]}).warnings)
            .toEqual(['communication slot 1: colibri_ethernet has no rs485 parameters, ignored']);
        const r = read({carrier: {type: 'acme', slots: 1, builtin: [{slot: 0, type: 'acme_io'}],
            comm: [{slot: 1, types: ['colibri_ethernet']}]}, comm: [{slot: 1, type: 'colibri_wifi'}]});
        expect(checkEnvironment(r.env!, catalog).warnings).toEqual([
            'built-in slot 0: unknown module type acme_io, the slot stays empty',
            'communication slot 1: does not take colibri_wifi, the slot stays empty',
        ]);
    });
});
