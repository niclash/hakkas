import {describe, expect, it} from 'vitest';
import {evaluate, fit, rollOver, roundTo} from '../src/evaluate.js';
import {seededRandom} from '../src/random.js';

describe('evaluating sources', () => {
    it('constants', () => {
        expect(evaluate({kind: 'constant', value: 3}, 99)).toBe(3);
        expect(evaluate({kind: 'constant', value: true}, 0)).toBe(true);
    });

    it('sine', () => {
        const s = {kind: 'sine', offset: 5, amplitude: 2, period: 4} as const;
        expect(evaluate(s, 0)).toBeCloseTo(5);
        expect(evaluate(s, 1)).toBeCloseTo(7);
        expect(evaluate(s, 3)).toBeCloseTo(3);
        expect(evaluate(s, 5)).toBeCloseTo(7);
    });

    it('square, high for the duty cycle’s part of each period', () => {
        const s = {kind: 'square', low: 0, high: 10, period: 2, duty: 25} as const;
        expect([0, 0.49, 0.5, 1.9, 2, 2.4, 2.6].map(t => evaluate(s, t))).toEqual([10, 10, 0, 0, 10, 10, 0]);
        const b = {kind: 'square', low: false, high: true, period: 1, duty: 0} as const;
        expect(evaluate(b, 0)).toBe(false);
        expect(evaluate({...b, duty: 100}, 0.99)).toBe(true);
    });

    it('ramp, once or repeating', () => {
        const once = {kind: 'ramp', from: 0, to: 10, period: 5, repeat: false} as const;
        expect([0, 2.5, 5, 50].map(t => evaluate(once, t))).toEqual([0, 5, 10, 10]);
        const saw = {...once, repeat: true};
        expect([0, 2.5, 5, 7.5].map(t => evaluate(saw, t))).toEqual([0, 5, 0, 5]);
        expect(evaluate({...once, from: 10, to: 0}, 1)).toBe(8);
    });

    it('counter', () => {
        expect([0, 0.49, 0.5, 10].map(t => evaluate({kind: 'counter', rate: 2}, t))).toEqual([0, 0, 1, 20]);
    });

    it('sequence, holding its last value or starting over', () => {
        const steps: [number, number][] = [[1, 10], [2, 20], [0.5, 30]];
        const hold = {kind: 'sequence', steps, repeat: false} as const;
        expect([0, 0.99, 1, 2.99, 3, 3.49, 3.5, 100].map(t => evaluate(hold, t))).toEqual([10, 10, 20, 20, 30, 30, 30, 30]);
        const loop = {...hold, repeat: true};
        expect([3.5, 4.5, 7].map(t => evaluate(loop, t))).toEqual([10, 20, 10]);
        expect(evaluate({kind: 'sequence', steps: [[1, true], [1, false]], repeat: true}, 1.5)).toBe(false);
    });

    it('not a pulse train, which is measured', () => {
        expect(() => evaluate({kind: 'pulse', frequency: {kind: 'constant', value: 1},
            duty: {kind: 'constant', value: 50}}, 0)).toThrow(/measure it/);
    });
});

describe('fitting a value to its channel', () => {
    it('clamps and rounds to the resolution, without binary fractions', () => {
        expect(fit(5.01234, {min: 0, max: 10, resolution: 0.001})).toBe(5.012);
        expect(fit(0.1 + 0.2, {resolution: 0.1})).toBe(0.3);
        expect(fit(-3, {min: 0, max: 10})).toBe(0);
        expect(fit(12, {min: 0, max: 10, resolution: 0.01})).toBe(10);
        expect(fit(21.456, {resolution: 0.05})).toBe(21.45);
        expect(fit(1234, {resolution: 10})).toBe(1230);
        expect(fit(7, {})).toBe(7);
    });

    it('stays within a bound that is not a multiple of the resolution', () => {
        expect(fit(10, {min: 0, max: 10, resolution: 4})).toBe(10);
        expect(fit(9.9, {min: 0, max: 10, resolution: 4})).toBe(8);
    });

    it('rounds with small and exponent-written resolutions', () => {
        expect(roundTo(1.23456789, 1e-7)).toBe(1.2345679);
        expect(roundTo(1.25, 0.5)).toBe(1.5);
        expect(roundTo(-0.004, 0.01) === 0).toBe(true);
    });
});

describe('rolling over', () => {
    it('counts from min to max and starts again', () => {
        expect([0, 1, 4294967295, 4294967296, 4294967297].map(n => rollOver(n, 0, 4294967295)))
            .toEqual([0, 1, 4294967295, 0, 1]);
        expect([0, 2, 3, 7].map(n => rollOver(n, 10, 12))).toEqual([10, 12, 10, 11]);
        expect(rollOver(5)).toBe(5);
        expect(rollOver(-1, 0, 9)).toBe(9);
    });
});

describe('the seeded random generator', () => {
    it('gives the same numbers for the same seed, in [0, 1)', () => {
        const a = seededRandom(42), b = seededRandom(42), c = seededRandom(43);
        const xs = Array.from({length: 1000}, a);
        expect(Array.from({length: 1000}, b)).toEqual(xs);
        expect(Array.from({length: 1000}, c)).not.toEqual(xs);
        expect(xs.every(x => x >= 0 && x < 1)).toBe(true);
        expect(Math.abs(xs.reduce((s, x) => s + x, 0) / xs.length - 0.5)).toBeLessThan(0.05);
    });
});
