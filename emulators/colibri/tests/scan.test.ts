import {describe, expect, it} from 'vitest';
import {loadCatalog} from '../src/catalog.js';
import {Checker, type JsonObject} from '../src/check.js';
import {readEnvironment} from '../src/environment.js';
import {createLogger} from '../src/log.js';
import {BUILTIN_DATA} from '../src/package.js';
import {Scanner, type Turn} from '../src/scan.js';
import {System, type OutputChannel, type SourcedChannel} from '../src/system.js';

const catalog = await loadCatalog([BUILTIN_DATA], createLogger('error', () => {}));

// A scanner on a fake clock (ms) that records its turns and warnings.
function build(env: JsonObject = {}) {
    let now = 1000;
    const c = new Checker();
    const environment = readEnvironment(c, {carrier: 'colibri_8', modules, ...env});
    expect(c.errors).toEqual([]);
    const {system} = System.build('lab', environment!, catalog, () => now, () => 0.5);
    const turns: Turn[] = [];
    const warnings: string[] = [];
    const log = createLogger('warn', line => warnings.push(line));
    const scanner = new Scanner(system, environment!, () => now, log, t => turns.push(t));
    return {
        system, scanner, turns, warnings,
        // Moves the clock on and serves what is due.
        run(ms: number) {
            now += ms;
            scanner.poll();
        },
        served: () => turns.map(t => t.slot.index),
        output: (key: string) => system.item(key) as OutputChannel,
        input: (key: string) => system.item(key) as SourcedChannel,
    };
}

const modules: JsonObject[] = [
    {slot: 1, type: 'colibri_aiv'},
    {slot: 3, type: 'colibri_ssr'},
];

describe('the scan cycle', () => {
    it('serves one slot per cycle time, empty slots too, slot 0 first', () => {
        const t = build();
        t.scanner.poll();
        expect(t.served()).toEqual([0]);
        t.run(9);
        expect(t.served()).toEqual([0]);
        t.run(1);
        expect(t.served()).toEqual([0, 1]);
        t.run(70);
        expect(t.served()).toEqual([0, 1, 2, 3, 4, 5, 6, 7, 0]);
    });

    it('keeps to its deadlines: a late turn is served at once and the next is not moved', () => {
        const t = build();
        t.scanner.poll();
        t.run(13);                  // turn 1 was due at 10
        expect(t.served()).toEqual([0, 1]);
        t.run(7);                   // turn 2 is due at 20, not 23
        expect(t.served()).toEqual([0, 1, 2]);
        t.run(35);                  // at 55: turns 3, 4 and 5 at once
        expect(t.served()).toEqual([0, 1, 2, 3, 4, 5]);
    });

    it('skips turns more than a whole cycle behind, warning at most once a minute', () => {
        const t = build();
        t.scanner.poll();
        t.run(1000);                // turns 1-100 due: the last 8 served, 92 skipped
        expect(t.served()).toEqual([0, 5, 6, 7, 0, 1, 2, 3, 4]);
        expect(t.warnings).toEqual(['warn scan cycle: 92 turns skipped, more than a whole cycle late']);
        t.run(1000);                // 92 more skipped, not yet warned about
        expect(t.warnings).toHaveLength(1);
        t.run(60_000);
        expect(t.warnings).toEqual([expect.anything(),
            'warn scan cycle: 6084 turns skipped, more than a whole cycle late']);
    });

    it('takes a new cycle time from the next turn', () => {
        const t = build();
        t.scanner.poll();           // turn 0 at 0; turn 1 due at 10
        t.scanner.setCycleMs(20);
        t.run(10);
        expect(t.served()).toEqual([0, 1]);
        t.run(10);
        expect(t.served()).toEqual([0, 1]);
        t.run(10);                  // turn 2 at 30
        expect(t.served()).toEqual([0, 1, 2]);
    });

    it('samples a slot\'s inputs only at its turn', () => {
        const t = build({inputs: {'slot1.ai.1': {source: {ramp: {from: 0, to: 10, period: 1, repeat: false}}}}});
        t.scanner.poll();
        t.run(10);
        const ai = t.input('slot1.ai.1');
        const first = ai.value;
        expect(t.turns[1]!.changed).toContain(ai);
        t.run(70);                  // slots 2-7 and 0: slot 1 not served
        expect(ai.value).toBe(first);
        t.run(10);
        expect(ai.value).not.toBe(first);
    });
});

describe('the write queues', () => {
    it('apply the writes at the module\'s turn, in order, and the last one stays', () => {
        const t = build();
        t.scanner.poll();
        const dq1 = t.output('slot3.dq.1'), dq2 = t.output('slot3.dq.2');
        expect(t.scanner.write(dq1, true)).toBe('queued');
        expect(t.scanner.write(dq2, true)).toBe('queued');
        expect(t.scanner.write(dq1, false)).toBe('queued');
        t.run(20);                  // slots 1 and 2
        expect([dq1.value, dq2.value]).toEqual([false, false]);
        t.run(10);
        expect(t.turns.at(-1)!.applied).toEqual([dq1, dq2, dq1]);
        expect([dq1.value, dq2.value]).toEqual([false, true]);
        expect(t.scanner.stats().parts.slot3).toEqual({queue: 0, queueMax: 3, dropped: 0});
    });

    it('drop a write when the queue is full', () => {
        const t = build({queueSize: 2});
        const dq1 = t.output('slot3.dq.1');
        expect([true, false, true, false].map(v => t.scanner.write(dq1, v)))
            .toEqual(['queued', 'queued', 'dropped', 'dropped']);
        t.scanner.poll();
        t.run(30);
        expect(dq1.value).toBe(false);
        const stats = t.scanner.stats();
        expect(stats.parts.slot3).toEqual({queue: 0, queueMax: 2, dropped: 2});
        expect(stats.channels['slot3.dq.1']).toEqual({received: 4, queued: 2, dropped: 2});
    });

    it('take a new size at once; a smaller one keeps the writes it holds', () => {
        const t = build();
        const dq1 = t.output('slot3.dq.1');
        for (let i = 0; i < 4; i++)
            t.scanner.write(dq1, i % 2 === 0);
        t.scanner.setQueueSize(3);
        expect(t.scanner.write(dq1, true)).toBe('dropped');
        expect(t.scanner.stats().parts.slot3!.queue).toBe(4);
        t.scanner.setQueueSize(5);
        expect(t.scanner.write(dq1, true)).toBe('queued');
    });
});

describe('the counters', () => {
    it('count the changes sent per input, and only modules\' parts', () => {
        const t = build({inputs: {'slot1.ai.1': {source: {ramp: {from: 0, to: 10, period: 1, repeat: true}}}}});
        t.scanner.poll();
        for (let i = 0; i < 25; i++)    // slot 1 served 4 times
            t.run(10);
        const stats = t.scanner.stats();
        expect(stats.channels['slot1.ai.1']).toEqual({sent: 4});
        expect(stats.channels['slot1.ai.2']).toEqual({sent: 1});    // its constant: the first value
        expect(Object.keys(stats.parts)).toEqual(['slot0', 'slot1', 'slot3']);
    });

    it('are set to 0 by a reset, a queue\'s length kept', () => {
        const t = build({queueSize: 1});
        const dq1 = t.output('slot3.dq.1');
        t.scanner.write(dq1, true);
        t.scanner.write(dq1, false);
        t.scanner.poll();
        t.scanner.resetStats();
        const stats = t.scanner.stats();
        expect(stats.parts.slot3).toEqual({queue: 1, queueMax: 1, dropped: 0});
        expect(stats.channels['slot3.dq.1']).toEqual({received: 0, queued: 0, dropped: 0});
        expect(stats.channels['slot1.ai.1']).toEqual({sent: 0});
    });

    it('refuse a channel of another system', () => {
        const a = build(), b = build();
        expect(() => a.scanner.write(b.output('slot3.dq.1'), true)).toThrow(RangeError);
    });
});

describe('on the event loop', () => {
    it('serves turns on a timer until stopped', async () => {
        const c = new Checker();
        const env = readEnvironment(c, {carrier: 'colibri_8', cycleMs: 2})!;
        const {system} = System.build('lab', env, catalog, () => performance.now(), () => 0.5);
        const turns: number[] = [];
        const scanner = new Scanner(system, env, () => performance.now(), createLogger('error', () => {}),
            t => turns.push(t.slot.index));
        scanner.start();
        await new Promise(resolve => setTimeout(resolve, 50));
        scanner.stop();
        const count = turns.length;
        expect(count).toBeGreaterThan(5);
        expect(turns.slice(0, 9)).toEqual([0, 1, 2, 3, 4, 5, 6, 7, 0]);
        await new Promise(resolve => setTimeout(resolve, 20));
        expect(turns.length).toBe(count);
    });
});
