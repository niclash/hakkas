// hakkas' thermostat example (hakkas plan X2, examples/thermostat/, the User's Manual's
// Chapter 4) with the real hakkas against the emulated Colibri 8 of
// examples/thermostat/thermostat.json: the heater switched by the room temperature around the
// office hours' setpoint and the setback's. The office hours are adapted to the moment the test
// runs (hakkas has no clock to set): always or never. Skipped when hakkas is not built
// (tests/example-rig.ts).
import {afterEach, beforeEach, describe, expect, it} from 'vitest';
import type {JsonObject} from '../src/check.js';
import {ExampleRig, HAKKAS_BUILT, readExample, withValues} from './example-rig.js';

const {process: example, environment} = readExample('thermostat', 'Thermostat.process.json', 'thermostat.json');

const ROOM = 'slot1.ai.1', HEATER = 'slot2.dq';

const graph = (occupied: boolean): JsonObject => withValues(example.graph, [
    ['office_hours', 'range', occupied ? '00:00-24:00' : '00:00-00:00'],
    ['workdays', 'days', '1-7'],
]);

let rig: ExampleRig;
const room = (t: number) => rig.system.set(`${ROOM}.source`, t);
const heater = () => rig.out(HEATER);

beforeEach(async () => {
    rig = new ExampleRig('thermostat', environment, [HEATER]);
    await rig.listen();
});

afterEach(() => rig.close());

describe.skipIf(!HAKKAS_BUILT)('the thermostat example', () => {
    it('loads with both its bindings', async () => {
        rig.serve({[ROOM]: 19.5});
        await rig.startHakkas(graph(true), example.bindings);
        expect(rig.log).toContainEqual(expect.stringMatching(/2 bindings applied from .*, 0 skipped/));
        expect(rig.problems()).toEqual([]);
    }, 20_000);

    it('during office hours: on below 20.5 °C, off above 21.5 °C, unchanged in between', async () => {
        rig.serve({[ROOM]: 19.5});
        await rig.startHakkas(graph(true), example.bindings);
        await rig.until(() => heater() === true, 'the heater to switch on');
        room(21);
        await new Promise(r => setTimeout(r, 300));
        expect(heater()).toBe(true);
        room(22);
        await rig.until(() => heater() === false, 'the heater to switch off');
        room(21);
        await new Promise(r => setTimeout(r, 300));
        expect(heater()).toBe(false);
        room(20.4);
        await rig.until(() => heater() === true, 'the heater to switch on again');
    }, 20_000);

    it('outside office hours: the setback, 17 °C', async () => {
        rig.serve({[ROOM]: 19.5});
        await rig.startHakkas(graph(false), example.bindings);
        await rig.until(() => rig.log.some(l => /started/.test(l)), 'the process to start');
        await new Promise(r => setTimeout(r, 500));
        expect(heater()).toBe(false);
        room(16);
        await rig.until(() => heater() === true, 'the heater to switch on');
        room(18);
        await rig.until(() => heater() === false, 'the heater to switch off');
    }, 20_000);
});
