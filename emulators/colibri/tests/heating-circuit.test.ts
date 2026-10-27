// hakkas' heating circuit example (hakkas plan X3, examples/heating-circuit/, the User's Manual's
// Chapter 12) with the real hakkas against the emulated Colibri 8 of
// examples/heating-circuit/heating.json: the shunt valve after the start delay, the night
// setback, the summer stop and the pump's exercise. The calendar settings are adapted to the
// moment the test runs (hakkas has no clock to set), the start delay shortened. Skipped when
// hakkas is not built (tests/example-rig.ts).
import {afterEach, beforeEach, describe, expect, it} from 'vitest';
import type {JsonObject} from '../src/check.js';
import {ExampleRig, HAKKAS_BUILT, readExample, withValues} from './example-rig.js';

const {process: example, types, environment} =
    readExample('heating-circuit', 'Heating Circuit.process.json', 'heating.json');

const OUTDOOR = 'slot1.ai.1', SUPPLY = 'slot1.ai.2', VALVE = 'slot2.aq.1', PUMP = 'slot3.dq';

const graph = (o: {comfort?: boolean, exercise?: boolean} = {}): JsonObject => withValues(example.graph, [
    ['comfort_hours', 'range', o.comfort === false ? '00:00-00:00' : '00:00-24:00'],
    ['exercise_day', 'days', '1-7'],
    ['exercise_time', 'range', o.exercise ? '00:00-24:00' : '00:00-00:00'],
    ['start_delay', 'milliseconds', 1500],
]);

let rig: ExampleRig;
const volts = () => rig.out(VALVE) as number;
const pump = () => rig.out(PUMP);
const set = (key: string, t: number) => rig.system.set(`${key}.source`, t);
const start = (g: JsonObject) => rig.startHakkas(g, example.bindings, types);

beforeEach(async () => {
    rig = new ExampleRig('heating', environment, [VALVE, PUMP]);
    await rig.listen();
});

afterEach(() => rig.close());

describe.skipIf(!HAKKAS_BUILT)('the heating circuit example', () => {
    it('loads with its composite type and its four bindings', async () => {
        rig.serve({[OUTDOOR]: 2, [SUPPLY]: 38});
        await start(graph());
        expect(rig.log).toContainEqual(expect.stringMatching(/1 composite type loaded/));
        expect(rig.log).toContainEqual(expect.stringMatching(/4 bindings applied from .*, 0 skipped/));
        expect(rig.problems()).toEqual([]);
    }, 20_000);

    it('the valve stays closed during the start delay, then regulates the supply water', async () => {
        // Outdoor 2 °C: the curve gives 42.3 °C. The supply water is much colder: the valve opens
        // (from 0, bumpless, then by the integral).
        rig.serve({[OUTDOOR]: 2, [SUPPLY]: 30});
        await start(graph());
        await rig.until(() => pump() === true, 'the pump');
        const started = Date.now();
        await rig.until(() => volts() > 0.2, 'the valve to open');
        expect(Date.now() - started).toBeGreaterThan(1000);
        // Too warm: it closes.
        set(SUPPLY, 55);
        await rig.until(() => volts() === 0, 'the valve to close');
    }, 20_000);

    it('at night the setpoint is 5 °C lower', async () => {
        // 40 °C is below the day's 42.3 °C but above the night's 37.3 °C.
        rig.serve({[OUTDOOR]: 2, [SUPPLY]: 40});
        await start(graph({comfort: false}));
        await rig.until(() => pump() === true, 'the pump');
        await new Promise(r => setTimeout(r, 3000));      // past the start delay, a few samples
        expect(volts()).toBe(0);
    }, 20_000);

    it('summer stop: no heating above 18 °C outdoors, until below 16 °C', async () => {
        rig.serve({[OUTDOOR]: 20, [SUPPLY]: 30});
        await start(graph());
        await rig.until(() => rig.log.some(l => /started/.test(l)), 'the process to start');
        await new Promise(r => setTimeout(r, 2500));
        expect(pump()).toBe(false);
        expect(volts()).toBe(0);
        set(OUTDOOR, 17);
        await new Promise(r => setTimeout(r, 500));
        expect(pump()).toBe(false);
        // Outdoor 10 °C: the curve gives 31.7 °C, and the supply water is at 20 °C.
        set(SUPPLY, 20);
        set(OUTDOOR, 10);
        await rig.until(() => pump() === true, 'the pump to start');
        await rig.until(() => volts() > 0.2, 'the valve to open');
    }, 20_000);

    it('the pump exercise runs the pump in summer, with the valve closed', async () => {
        rig.serve({[OUTDOOR]: 20, [SUPPLY]: 30});
        await start(graph({exercise: true}));
        await rig.until(() => pump() === true, 'the pump');
        await new Promise(r => setTimeout(r, 2500));
        expect(volts()).toBe(0);
    }, 20_000);
});
