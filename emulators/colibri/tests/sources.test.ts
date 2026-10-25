import {describe, expect, it} from 'vitest';
import {Checker, type Json} from '../src/check.js';
import {acceptedSources, readSource, type RowType, type Source} from '../src/sources.js';

function read(v: Json, type: RowType = 'number', accepted = acceptedSources(type, 1)): Source {
    const c = new Checker();
    const s = readSource(c, v, 'src', type, accepted);
    expect(c.errors).toEqual([]);
    return s!;
}

function refuse(v: Json, type: RowType = 'number', accepted = acceptedSources(type, 1)): string[] {
    const c = new Checker();
    expect(readSource(c, v, 'src', type, accepted)).toBeUndefined();
    expect(c.errors.length).toBeGreaterThan(0);
    return c.errors;
}

describe('sources', () => {
    it('are accepted per row type', () => {
        expect(acceptedSources('number', 0.01)).toEqual(['constant', 'sine', 'square', 'ramp', 'sequence']);
        expect(acceptedSources('number', 1)).toContain('counter');
        expect(acceptedSources('number')).not.toContain('counter');
        expect(acceptedSources('bool')).toEqual(['constant', 'square', 'sequence']);
        expect(acceptedSources('pulse')).toEqual(['pulse']);
    });

    it('read constants of the row type', () => {
        expect(read(5.5)).toEqual({kind: 'constant', value: 5.5});
        expect(read(true, 'bool')).toEqual({kind: 'constant', value: true});
        expect(refuse(true)).toEqual(['src: expected a number']);
        expect(refuse(1, 'bool')).toEqual(['src: expected true or false']);
        expect(refuse('5')[0]).toMatch(/expected a source/);
        expect(refuse(null)[0]).toMatch(/expected a source/);
    });

    it('read the functions', () => {
        expect(read({sine: {offset: 5, amplitude: 2, period: 60}}))
            .toEqual({kind: 'sine', offset: 5, amplitude: 2, period: 60});
        expect(read({square: {low: 1, high: 2, period: 1, duty: 25}}))
            .toEqual({kind: 'square', low: 1, high: 2, period: 1, duty: 25});
        expect(read({square: {period: 2, duty: 50}}, 'bool'))
            .toEqual({kind: 'square', low: false, high: true, period: 2, duty: 50});
        expect(read({ramp: {from: 0, to: 10, period: 5}}))
            .toEqual({kind: 'ramp', from: 0, to: 10, period: 5, repeat: false});
        expect(read({counter: {rate: 3}})).toEqual({kind: 'counter', rate: 3});
        expect(read({sequence: {steps: [[1, 0], [0.5, 3]], repeat: true}}))
            .toEqual({kind: 'sequence', steps: [[1, 0], [0.5, 3]], repeat: true});
        expect(read({sequence: {steps: [[2, true]]}}, 'bool'))
            .toEqual({kind: 'sequence', steps: [[2, true]], repeat: false});
        expect(read({pulse: {frequency: 10, duty: {ramp: {from: 0, to: 100, period: 10}}}}, 'pulse')).toEqual({
            kind: 'pulse', frequency: {kind: 'constant', value: 10},
            duty: {kind: 'ramp', from: 0, to: 100, period: 10, repeat: false},
        });
    });

    it('refuse a source the row does not accept', () => {
        expect(refuse({counter: {rate: 1}}, 'number', acceptedSources('number', 0.1))[0])
            .toMatch(/source counter not accepted here/);
        expect(refuse({sine: {offset: 0, amplitude: 1, period: 1}}, 'bool')[0]).toMatch(/not accepted/);
        expect(refuse(5, 'pulse')[0]).toMatch(/not accepted/);
        expect(refuse({pulse: {frequency: {counter: {rate: 1}}, duty: 50}}, 'pulse'))
            .toEqual([expect.stringMatching(/^src.pulse.frequency: source counter not accepted/)]);
    });

    it('refuse malformed functions with the path of the problem', () => {
        expect(refuse({sine: {offset: 0, amplitude: 1}})).toEqual(['src.sine.period: missing']);
        expect(refuse({sine: {offset: 0, amplitude: 1, period: 0}})).toEqual(['src.sine.period: expected a number > 0']);
        expect(refuse({square: {low: 0, high: 1, period: 1, duty: 101}}))
            .toEqual(['src.square.duty: expected a number from 0 to 100']);
        expect(refuse({square: {low: 0, high: true, period: 1, duty: 5}})).toEqual(['src.square.high: expected a number']);
        expect(refuse({counter: {rate: -1}})).toEqual(['src.counter.rate: expected a number ≥ 0']);
        expect(refuse({sequence: {steps: []}})).toEqual(['src.sequence.steps: expected at least one step']);
        expect(refuse({sequence: {steps: [[0, 1]]}})).toEqual(['src.sequence.steps[0]: expected a duration > 0 in seconds']);
        expect(refuse({sequence: {steps: [[1, true]]}})).toEqual(['src.sequence.steps[0]: expected a number value']);
        expect(refuse({sequence: {steps: [1]}})).toEqual(['src.sequence.steps[0]: expected [seconds, value]']);
        expect(refuse({pulse: {frequency: -1, duty: 50}}, 'pulse')).toEqual(['src.pulse.frequency: expected a number ≥ 0']);
        expect(refuse({pulse: {frequency: 1, duty: 150}}, 'pulse')).toEqual(['src.pulse.duty: expected a number from 0 to 100']);
        expect(refuse({sine: 5})).toEqual(['src.sine: expected an object']);
        expect(refuse({sine: {}, ramp: {}})[0]).toMatch(/one member/);
        expect(refuse({wobble: {}})[0]).toMatch(/expected a source/);
    });

    it('warn about unknown members', () => {
        const c = new Checker();
        readSource(c, {counter: {rate: 1, phase: 2}}, 'src', 'number', ['counter']);
        expect(c.errors).toEqual([]);
        expect(c.warnings).toEqual(['src.counter.phase: unknown member, ignored']);
    });
});
