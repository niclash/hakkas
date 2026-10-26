import {describe, expect, it} from 'vitest';
import {loadCatalog} from '../src/catalog.js';
import {type JsonObject} from '../src/check.js';
import {CopyError, deviceEndpoint, environmentFrom} from '../src/device.js';
import {createLogger} from '../src/log.js';
import {BUILTIN_DATA} from '../src/package.js';

// Copying from a device (spec §5.3, plan task E16): where the device's WebSocket is, and the
// environment a description makes.
const catalog = await loadCatalog([BUILTIN_DATA], createLogger('error', () => {}));
const mcu6 = catalog.mcu.get('colibri_mcu6')!.def;

describe('the device\'s address', () => {
    it('gives its WebSocket, the next port, and its own origin', () => {
        expect(deviceEndpoint('http://192.168.1.40:8080/'))
            .toEqual({ws: 'ws://192.168.1.40:8081/', origin: 'http://192.168.1.40:8080'});
        expect(deviceEndpoint('http://colibri.local/'))
            .toEqual({ws: 'ws://colibri.local:81/', origin: 'http://colibri.local'});
        expect(deviceEndpoint('https://colibri.local'))
            .toEqual({ws: 'wss://colibri.local:444/', origin: 'https://colibri.local'});
        expect(deviceEndpoint('http://[fe80::1]:80/')).toEqual({ws: 'ws://[fe80::1]:81/', origin: 'http://[fe80::1]'});
    });

    it('is refused when it is no web address', () => {
        for (const a of ['', 'colibri.local', 'ftp://x/', 'http://u:p@x/', 'http://x:65535/'])
            expect(() => deviceEndpoint(a), a).toThrow(CopyError);
    });
});

const device = (carrier: JsonObject, modules: JsonObject[] = []): JsonObject => ({
    name: 'plant', outPorts: [], inPorts: [],
    modules: [{slot: 0, builtin: true, type: 'carrier', ports: []}, ...modules], carrier});

describe('the environment a description makes', () => {
    it('is the catalog\'s carrier, the modules with serials, the communication modules with parameters', () => {
        const ip = {mode: 'static', address: '10.0.0.7', prefix: 24};
        const {env, warnings} = environmentFrom(device(
            {type: 'colibri_8', slots: 7, builtin: [{slot: 0}], comm: [{slot: 1, type: 'colibri_ethernet', ip}],
                mcu: {type: 'colibri_mcu6'}},
            [{slot: 1, type: 'colibri_aiv', serial: 'A-1', ports: []}, {slot: 4, type: 'colibri_ssr', ports: []}]),
        catalog, mcu6);
        expect(warnings).toEqual([]);
        expect(env.carrier).toBe('colibri_8');
        expect(env.modules).toEqual([{slot: 1, type: 'colibri_aiv', serial: 'A-1'}, {slot: 4, type: 'colibri_ssr'}]);
        expect(env.comm).toEqual([{slot: 1, type: 'colibri_ethernet', ip}]);
        expect(env.inputs).toEqual({});
    });

    it('warns about what the catalog lacks, leaving its slot empty, and another MCU module', () => {
        const {env, warnings} = environmentFrom(device(
            {type: 'colibri_8', slots: 7, builtin: [{slot: 0}], comm: [{slot: 1, type: 'colibri_5g'}],
                mcu: {type: 'colibri_mcu2'}},
            [{slot: 2, type: 'colibri_flux', ports: []}]), catalog, mcu6);
        expect(env.modules).toEqual([]);
        expect(env.comm).toEqual([]);
        expect(warnings).toEqual([
            'slot 2: module type colibri_flux is not in the catalog: left empty',
            'communication slot 1: type colibri_5g is not in the catalog: left empty',
            'the device\'s MCU module is colibri_mcu2; the emulator names the one --mcu gives (colibri_mcu6)',
        ]);
    });

    it('makes a carrier definition for a carrier type the catalog lacks, kept in the environment', () => {
        const {env, warnings} = environmentFrom(device(
            {type: 'colibri_16pi', name: 'Colibri 16pi', slots: 15, builtin: [{slot: 0}],
                power: {voltage: {min: 12, max: 24}, ac: false, dc: true},
                comm: [{slot: 1, type: 'colibri_wifi', wifi: {ssid: 'x'}}]},
            [{slot: 9, type: 'colibri_pt1000', ports: []}]), catalog);
        expect(env.carrier).toMatchObject({type: 'colibri_16pi', name: 'Colibri 16pi', slots: 15,
            builtin: [{slot: 0, type: 'carrier'}], comm: [{slot: 1, types: ['colibri_wifi']}]});
        expect(env.modules).toEqual([{slot: 9, type: 'colibri_pt1000'}]);
        expect(env.comm).toEqual([{slot: 1, type: 'colibri_wifi', wifi: {ssid: 'x'}}]);
        expect(warnings).toEqual(['carrier type colibri_16pi is not in the catalog: its definition was made from the ' +
            'description and is kept in the environment']);
        // A power of another form is left out rather than no copy.
        const odd = environmentFrom(device({type: 'other', slots: 2, builtin: [], comm: [], power: {current: 'lots'}}),
            catalog);
        expect(odd.warnings).toContain('the carrier\'s power is not of the emulator\'s form: left out');
    });

    it('is refused for an environment without a carrier', () => {
        expect(() => environmentFrom({name: 'flat', outPorts: [], inPorts: [], modules: []}, catalog))
            .toThrow('Not a Colibri environment');
    });
});
