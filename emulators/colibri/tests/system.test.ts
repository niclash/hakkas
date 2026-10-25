import {describe, expect, it} from 'vitest';
import {loadCatalog} from '../src/catalog.js';
import {Checker, type JsonObject} from '../src/check.js';
import {readEnvironment} from '../src/environment.js';
import {createLogger} from '../src/log.js';
import {BUILTIN_DATA} from '../src/package.js';
import {ModelError, System, type MeasuredChannel, type OutputChannel, type SourcedChannel} from '../src/system.js';

const catalog = await loadCatalog([BUILTIN_DATA], createLogger('error', () => {}));

// A system on a fake clock (ms) and a random generator that returns what the test queues
// (0.5, no noise, when nothing is queued).
function build(env: JsonObject) {
    let now = 1000;
    const randoms: number[] = [];
    const c = new Checker();
    const environment = readEnvironment(c, {carrier: 'colibri_8', ...env});
    expect(c.errors).toEqual([]);
    const {system, warnings} = System.build('lab', environment!, catalog, () => now, () => randoms.shift() ?? 0.5);
    return {
        system, warnings, randoms,
        tick(ms: number) {
            now += ms;
        },
        input: (key: string) => system.item(key) as SourcedChannel,
        measured: (key: string) => system.item(key) as MeasuredChannel,
        value: (key: string) => (system.item(key) as SourcedChannel).value,
    };
}

const modules: JsonObject[] = [
    {slot: 1, type: 'colibri_aiv', serial: 'EMU-0001'},
    {slot: 2, type: 'colibri_dii'},
    {slot: 3, type: 'colibri_ssr'},
    {slot: 4, type: 'colibri_dp1'},
    {slot: 5, type: 'colibri_pid1'},
    {slot: 6, type: 'colibri_flux'},
];

describe('the emulated system', () => {
    it('has every slot of the carrier, filled or not', () => {
        const {system} = build({modules});
        expect(system.slots.map(s => [s.part, s.builtin, s.type, s.module?.type])).toEqual([
            ['slot0', true, 'carrier', 'carrier'],
            ['slot1', false, 'colibri_aiv', 'colibri_aiv'],
            ['slot2', false, 'colibri_dii', 'colibri_dii'],
            ['slot3', false, 'colibri_ssr', 'colibri_ssr'],
            ['slot4', false, 'colibri_dp1', 'colibri_dp1'],
            ['slot5', false, 'colibri_pid1', 'colibri_pid1'],
            ['slot6', false, 'colibri_flux', undefined],
            ['slot7', false, undefined, undefined],
        ]);
        expect(system.slots[1]!.serial).toBe('EMU-0001');
        expect(system.slots[6]!.inputs).toEqual([]);
    });

    it('names keys, ports and labels per slot, with an index only for a group of several', () => {
        const {system} = build({modules});
        const ai1 = system.item('slot1.ai.1')!;
        expect(ai1).toMatchObject({key: 'slot1.ai.1', port: 'slot1-ai', index: 1, label: 'slot1-ai[1]'});
        const dp = system.item('slot4.ai')!;
        expect(dp).toMatchObject({key: 'slot4.ai', port: 'slot4-ai', label: 'slot4-ai'});
        expect('index' in dp).toBe(false);
        expect(system.channel('slot1-ai', 2)!.key).toBe('slot1.ai.2');
        expect(system.channel('slot4-ai')!.key).toBe('slot4.ai');
        expect(system.channel('slot4-ai', 1)).toBeUndefined();
        expect(system.channel('slot3-dq', 2)!.kind).toBe('output');
        expect(system.item('slot2.in.1')!.kind).toBe('signal');
        expect(system.channel('slot2-in', 1)).toBeUndefined();
        expect(system.item('slot5.sample_ms')!.kind).toBe('output');
    });

    it('keeps the communication modules its carrier takes, with their parameter groups', () => {
        const {system} = build({comm: [{slot: 1, type: 'colibri_wifi', ip: {mode: 'dhcp'}, wifi: {ssid: 'lab'},
            rs485: {bitrate: 9600}}]});
        expect(system.comm.map(c => [c.slot, c.def.type, c.params]))
            .toEqual([[1, 'colibri_wifi', {ip: {mode: 'dhcp'}, wifi: {ssid: 'lab'}}]]);
        expect(build({comm: [{slot: 1, type: 'colibri_zigbee'}]}).system.comm).toEqual([]);
    });

    it('starts inputs without a value and outputs at their defaults', () => {
        const {system} = build({modules});
        expect(system.state()).toMatchObject({
            'slot1.ai.1': null, 'slot1.ai.1.source': 0, 'slot1.ai.1.noise': 0.005,
            'slot3.dq.1': false, 'slot5.setpoint': 0,
            'slot2.in.1.source': {pulse: {frequency: 0, duty: 0}},
            'slot2.count.1': null, 'slot2.freq.1.noise': 0,
        });
        expect(system.state()).not.toHaveProperty('slot2.in.1');
        expect(system.state()).not.toHaveProperty('slot2.count.1.noise');
        expect(system.state()).not.toHaveProperty('slot2.di.1.noise');
        expect(system.state()).not.toHaveProperty('slot3.dq.1.source');
    });
});

describe('sampling a slot', () => {
    it('computes its inputs with noise, clamped and rounded, and returns those that changed', () => {
        const m = build({modules, inputs: {'slot1.ai.1': {source: 5, noise: 0.1}}});
        m.randoms.push(1, 0.5);                 // +0.1 on ai.1; ai.2 has its default noise, here none
        expect(m.system.sample(1).map(ch => [ch.key, ch.value])).toEqual([['slot1.ai.1', 5.1], ['slot1.ai.2', 0]]);
        m.randoms.push(0.5, 0.5);
        expect(m.system.sample(1)).toEqual([expect.objectContaining({key: 'slot1.ai.1', value: 5})]);
        m.randoms.push(0.5, 0.5);
        expect(m.system.sample(1)).toEqual([]);
        m.system.set('slot1.ai.1.source', 99);
        m.randoms.push(0.5, 0.5);
        expect(m.system.sample(1).map(ch => ch.value)).toEqual([10]);
        expect(m.system.sample(0)).toEqual([]);
        expect(() => m.system.sample(8)).toThrow(RangeError);
    });

    it('evaluates a function from when its source was set', () => {
        const m = build({modules});
        m.system.set('slot1.ai.1.noise', 0);
        m.tick(5000);
        m.system.set('slot1.ai.1.source', {ramp: {from: 0, to: 10, period: 10}});
        m.tick(2500);
        m.system.sample(1);
        expect(m.value('slot1.ai.1')).toBe(2.5);
    });

    it('rolls a counter source over its range, without noise', () => {
        const m = build({modules: [{slot: 1, type: 'colibri_aiv'}]});
        // No built-in module counts in whole units, so the test gives one a counter's range.
        const ch = m.input('slot1.ai.1');
        ch.def = {...ch.def, min: 0, max: 9, resolution: 1, sources: [...ch.def.sources, 'counter']};
        m.system.set('slot1.ai.1.source', {counter: {rate: 4}});
        m.tick(3000);
        m.randoms.push(1, 1);
        m.system.sample(1);
        expect(m.value('slot1.ai.1')).toBe(2);   // 12 counts: 0…9, 0, 1, 2
    });

    it('gives bool inputs their source’s value', () => {
        const m = build({modules: [{slot: 1, type: 'colibri_pt1000'}]});
        m.system.set('slot1.fault.2.source', {square: {period: 1, duty: 50}});
        m.system.sample(1);
        expect(m.value('slot1.fault.2')).toBe(true);
        m.tick(600);
        m.system.sample(1);
        expect(m.value('slot1.fault.2')).toBe(false);
        expect(m.value('slot1.fault.1')).toBe(false);
    });
});

describe('a pulse train', () => {
    function dii() {
        const m = build({modules: [{slot: 2, type: 'colibri_dii'}]});
        const read = () => {
            m.system.sample(2);
            return ['di', 'freq', 'duty', 'count'].map(q => m.measured(`slot2.${q}.1`).value);
        };
        return {...m, read};
    }

    it('is measured as state, frequency, duty cycle and count', () => {
        const m = dii();
        m.system.set('slot2.in.1.source', {pulse: {frequency: 10, duty: 30}});
        expect(m.read()).toEqual([true, 10, 30, 0]);
        m.tick(20);                                     // 0.2 periods: still high
        expect(m.read()).toEqual([true, 10, 30, 0]);
        m.tick(20);                                     // 0.4: low
        expect(m.read()[0]).toBe(false);
        m.tick(1060);                                   // 11 periods
        expect(m.read()).toEqual([true, 10, 30, 11]);
        expect(m.measured('slot2.count.2').value).toBe(0);
    });

    it('counts at its rate however rarely it is sampled', () => {
        const m = dii();
        m.system.set('slot2.in.1.source', {pulse: {frequency: 1000, duty: 50}});
        m.tick(80);
        expect(m.read()[3]).toBe(80);
        m.tick(7920);
        expect(m.read()[3]).toBe(8000);
    });

    it('is a steady level at frequency 0', () => {
        const m = dii();
        expect(m.read()).toEqual([false, 0, 0, 0]);
        m.system.set('slot2.in.1.source', {pulse: {frequency: 0, duty: 50}});
        expect(m.read()).toEqual([true, 0, 100, 0]);
        m.system.set('slot2.in.1.source', {pulse: {frequency: 0, duty: 49}});
        m.tick(10_000);
        expect(m.read()).toEqual([false, 0, 0, 0]);
    });

    it('keeps its count when its source changes', () => {
        const m = dii();
        m.system.set('slot2.in.1.source', {pulse: {frequency: 10, duty: 50}});
        m.tick(1000);
        m.system.set('slot2.in.1.source', {pulse: {frequency: 100, duty: 50}});
        m.tick(1000);
        expect(m.read()[3]).toBe(110);
    });

    it('integrates a changing frequency', () => {
        const m = dii();
        m.system.set('slot2.in.1.source', {pulse: {frequency: {ramp: {from: 0, to: 100, period: 10}}, duty: 50}});
        for (let i = 0; i < 125; i++) {               // 10 s in 80 ms turns
            m.tick(80);
            m.system.sample(2);
        }
        expect(m.measured('slot2.count.1').value).toBe(500);    // the ramp's area: 10 s × 50 Hz
        expect(m.measured('slot2.freq.1').value).toBe(100);
    });

    it('rolls its count over at 32 bits', () => {
        const m = dii();
        m.system.set('slot2.in.1.source', {pulse: {frequency: 1e6, duty: 50}});
        m.tick(4294967.296 * 1000 + 3000);              // 2³² pulses and 3 s more
        expect(m.read()[3]).toBe(3_000_000);
    });

    it('takes noise on its frequency and duty cycle', () => {
        const m = dii();
        m.system.set('slot2.in.1.source', {pulse: {frequency: 10, duty: 50}});
        m.system.set('slot2.freq.1.noise', 1);
        m.system.set('slot2.duty.1.noise', 2);
        m.randoms.push(1, 0);
        expect(m.read()).toEqual([true, 11, 48, 0]);
    });
});

describe('setting sources and noise', () => {
    it('refuses with the control protocol’s messages', () => {
        const {system} = build({modules});
        const refused = (key: string, value: unknown, message: string | RegExp) =>
            expect(() => system.set(key, value as never)).toThrow(message);
        refused('slot9.ai.1.source', 1, 'Unknown key: slot9.ai.1.source');
        refused('slot1.ai.1', 1, 'Not settable: slot1.ai.1');
        refused('slot1.ai.1.colour', 1, 'Unknown key: slot1.ai.1.colour');
        refused('slot3.dq.1.source', true, 'Not settable: slot3.dq.1.source');
        refused('slot3.dq.1.noise', 1, 'Not settable: slot3.dq.1.noise');
        refused('slot2.freq.1.source', 1, 'Unknown key: slot2.freq.1.source');
        refused('slot2.in.1.noise', 1, 'Unknown key: slot2.in.1.noise');
        refused('slot2.count.1.noise', 1, 'Unknown key: slot2.count.1.noise');
        refused('slot2.di.1.noise', 1, 'Unknown key: slot2.di.1.noise');
        refused('slot1.ai.1.source', {counter: {rate: 1}}, 'Source not accepted: slot1.ai.1.source');
        refused('slot2.in.1.source', 5, 'Source not accepted: slot2.in.1.source');
        refused('slot1.ai.1.source', {sine: {offset: 1, amplitude: 1, period: 0}},
            'Invalid source: slot1.ai.1.source: sine.period: expected a number > 0');
        refused('slot1.ai.1.source', 'five', /^Invalid source: slot1.ai.1.source: expected a source/);
        refused('slot1.ai.1.noise', -1, 'Invalid noise: slot1.ai.1.noise');
        refused('slot1.ai.1.noise', '1', 'Invalid noise: slot1.ai.1.noise');
        expect(() => system.set('slot1.ai.1', 1)).toThrow(ModelError);
    });

    it('shows them in the state as set', () => {
        const {system} = build({modules});
        system.set('slot1.ai.2.source', {sine: {offset: 5, amplitude: 1, period: 10}});
        system.set('slot1.ai.2.noise', 0);
        expect(system.state()).toMatchObject({
            'slot1.ai.2.source': {sine: {offset: 5, amplitude: 1, period: 10}}, 'slot1.ai.2.noise': 0});
    });
});

describe('the environment’s inputs', () => {
    it('are applied when the system is built', () => {
        const {system, warnings} = build({modules, inputs: {
            'slot1.ai.1': {source: 5, noise: 0.02},
            'slot2.in.1': {source: {pulse: {frequency: 10, duty: 50}}},
            'slot2.freq.2': {noise: 0.5},
        }});
        expect(warnings).toEqual([]);
        expect(system.state()).toMatchObject({'slot1.ai.1.source': 5, 'slot1.ai.1.noise': 0.02,
            'slot2.in.1.source': {pulse: {frequency: 10, duty: 50}}, 'slot2.freq.2.noise': 0.5});
    });

    it('that do not fit are left at their defaults, with a warning', () => {
        const {system, warnings} = build({modules, inputs: {
            'slot7.ai.1': {source: 5},
            'slot3.dq.1': {source: true},
            'slot1.ai.1': {source: {counter: {rate: 1}}, noise: 0.5},
            'slot1.ai.2': {noise: -1},
        }});
        expect(warnings).toEqual([
            'inputs.slot7.ai.1: no such input or signal, dropped',
            'inputs.slot3.dq.1: no such input or signal, dropped',
            'inputs.slot1.ai.1.source: Source not accepted: slot1.ai.1.source, the default kept',
            'inputs.slot1.ai.2.noise: Invalid noise: slot1.ai.2.noise, the default kept',
        ]);
        expect(system.state()).toMatchObject({'slot1.ai.1.source': 0, 'slot1.ai.1.noise': 0.5,
            'slot1.ai.2.noise': 0.005});
    });

    it('are persisted where they differ from the defaults', () => {
        const env = {modules, cycleMs: 20, note: 'kept', inputs: {'slot1.ai.1': {source: 5}, 'slot7.ai.1': {source: 1}}};
        const {system} = build(env);
        system.set('slot1.ai.2.noise', 0);
        system.set('slot2.in.2.source', {pulse: {frequency: 3, duty: 20}});
        system.set('slot1.ai.1.noise', 0.005);              // back to the default: not written
        const out = system.toEnvironment();
        expect(out.inputs).toEqual({
            'slot1.ai.1': {source: 5},
            'slot1.ai.2': {noise: 0},
            'slot2.in.2': {source: {pulse: {frequency: 3, duty: 20}}},
        });
        expect(out).toMatchObject({cycleMs: 20, carrier: 'colibri_8', extra: {note: 'kept'}});
        expect(out.modules).toEqual(modules);                // colibri_flux is kept
    });
});

describe('outputs', () => {
    it('take the value hakkas wrote', () => {
        const {system} = build({modules});
        const dq = system.channel('slot3-dq', 1) as OutputChannel;
        system.applyOutput(dq, true);
        expect(system.state()['slot3.dq.1']).toBe(true);
    });
});
