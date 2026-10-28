// hakkas' Air Handling Unit example (hakkas plan X1, examples/ahu/) with the real hakkas against
// the emulated Colibri 8 of examples/ahu/ahu.json: the seasons' regulators, a stopped unit,
// frost protection and Extended operation. The example's calendar settings are adapted to the
// moment the test runs (hakkas has no clock to set): the schedule always on or never, the
// season under test the current month, the pump exercise never. Skipped when hakkas is not
// built (tests/example-rig.ts).
import {afterEach, beforeEach, describe, expect, it} from 'vitest';
import type {JsonObject} from '../src/check.js';
import {ExampleRig, HAKKAS_BUILT, readExample, withValues} from './example-rig.js';

const {process: example, environment} = readExample('ahu', 'Air Handling Unit.process.json', 'ahu.json');

type Season = 'heating' | 'cooling' | 'neither';

// The example's process with its calendar settings for this test.
function graph(season: Season, running: boolean, extendedMs?: number): JsonObject {
    const month = new Date().getUTCMonth() + 1;           // hakkas' time zone is UTC by default
    const other = String(month % 12 + 1), another = String((month + 1) % 12 + 1);
    return withValues(example.graph, [
        ['workdays', 'days', '1-7'],
        ['work_hours', 'range', running ? '00:00-24:00' : '00:00-00:00'],
        ['heating_months', 'months', season === 'heating' ? String(month) : other],
        ['september', 'months', another],
        ['may', 'months', another],
        ['cooling_season', 'months', season === 'cooling' ? String(month) : other],
        ['exercise_time', 'range', '00:00-00:00'],
        ['power_up_time', 'in', 1],
        ...(extendedMs !== undefined ? [['extended_time', 'in', extendedMs / 3600000] as [string, string, unknown]] : []),
    ]);
}

let rig: ExampleRig;
const out = (key: string) => rig.out(key);
const volts = (key: string) => out(key) as number;
const until = (condition: () => boolean, what: string, ms?: number) => rig.until(condition, what, ms);
const serve = (inputs: Record<string, number>) => rig.serve(inputs);
const startHakkas = (g: JsonObject) => rig.startHakkas(g, example.bindings);
const press = (input: number, down: boolean) =>
    rig.system.set(`slot7.in.${input}.source`, {pulse: {frequency: 0, duty: down ? 100 : 0}});

// The outputs, by the example's names.
const FAN_S = 'slot6.dq.1', FAN_E = 'slot6.dq.2', PUMP_H = 'slot6.dq.3', PUMP_C = 'slot6.dq.4';
const VALVE_H = 'slot4.aq.1', VALVE_C = 'slot4.aq.2', EXCHANGER = 'slot5.aq.1';
// Outdoor 2 °C (the curve gives 19.8 °C), room at its setpoint; the coil's water warm.
const START = {'slot1.ai.1': 2, 'slot1.ai.2': 19.8, 'slot2.ai.1': 22, 'slot2.ai.2': 6,
    'slot3.ai.1': 30, 'slot3.ai.2': 21};

beforeEach(async () => {
    rig = new ExampleRig('ahu', environment, [VALVE_H, VALVE_C, EXCHANGER, FAN_S, PUMP_H, PUMP_C]);
    await rig.listen();
});

afterEach(() => rig.close());

describe.skipIf(!HAKKAS_BUILT)('the Air Handling Unit example', () => {
    it('loads with all its bindings and makes the DIO1 channels outputs', async () => {
        serve(START);
        await startHakkas(graph('heating', true));
        expect(rig.log).toContainEqual(expect.stringMatching(/19 bindings applied from .*, 0 skipped/));
        expect(rig.problems()).toEqual([]);
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
        rig.system.set('slot1.ai.2.source', 30);
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
        rig.system.set('slot1.ai.2.source', 30);
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
        rig.system.set('slot1.ai.2.source', 12);
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
        rig.system.set('slot3.ai.1.source', 3);
        await until(() => out(FAN_S) === false && out(FAN_E) === false, 'the fans to stop (after 5 s)', 10000);
        expect(out(PUMP_H)).toBe(true);
        await until(() => volts(VALVE_H) > 5, 'the heating valve to open wide', 15000);
        // Acknowledged while still cold: nothing.
        press(2, true);
        await new Promise(r => setTimeout(r, 300));
        press(2, false);
        // Warm again (above 20 °C), past the 5 s confirmation: still locked, the early
        // acknowledgement did not count.
        rig.system.set('slot3.ai.1.source', 25);
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
