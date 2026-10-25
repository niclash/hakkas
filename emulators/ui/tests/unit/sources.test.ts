import {describe, expect, it} from 'vitest';
import {decimals, describeSource, formatValue, kindOf, newSource, stepOf} from '@/sources';

describe('sources', () => {
    it('are told apart by their form', () => {
        expect(kindOf(5)).toBe('constant');
        expect(kindOf(true)).toBe('constant');
        expect(kindOf({sine: {offset: 1, amplitude: 1, period: 1}})).toBe('sine');
        expect(kindOf({a: 1, b: 2})).toBeUndefined();
        expect(kindOf(null)).toBeUndefined();
    });

    it('start within the row\'s range', () => {
        const row = {type: 'number' as const, min: 0, max: 10};
        expect(newSource('constant', row, 3)).toBe(3);
        expect(newSource('constant', row)).toBe(5);
        expect(newSource('sine', row)).toEqual({sine: {offset: 5, amplitude: 5, period: 10}});
        expect(newSource('square', row)).toEqual({square: {low: 0, high: 10, period: 2, duty: 50}});
        expect(newSource('ramp', {type: 'number', min: -50, max: 150})).toEqual({ramp: {from: -50, to: 150, period: 10, repeat: true}});
        expect(newSource('square', {type: 'bool'})).toEqual({square: {low: false, high: true, period: 2, duty: 50}});
        expect(newSource('constant', {type: 'bool'}, true)).toBe(true);
        expect(newSource('pulse', {type: 'pulse'})).toEqual({pulse: {frequency: 1, duty: 50}});
    });

    it('are described in a few words', () => {
        expect(describeSource(7.5, 'V')).toBe('7.5 V');
        expect(describeSource({sine: {offset: 5, amplitude: 2, period: 10}}, 'V')).toBe('sine 5 ± 2 V, 10 s');
        expect(describeSource({ramp: {from: 0, to: 10, period: 5, repeat: false}})).toBe('ramp 0 → 10, 5 s');
        expect(describeSource({square: {low: false, high: true, period: 2, duty: 25}})).toBe('square false/true, 2 s, 25 %');
        expect(describeSource({sequence: {steps: [[1, 0], [2, 1]], repeat: true}})).toBe('sequence of 2, repeating');
        expect(describeSource({pulse: {frequency: {ramp: {from: 0, to: 100, period: 10, repeat: true}}, duty: 50}}))
            .toBe('pulses ramp 0 → 100 Hz, 10 s, repeating, duty 50 %');
    });
});

describe('values', () => {
    it('show their resolution\'s decimals', () => {
        expect(decimals(0.001)).toBe(3);
        expect(decimals(0.1)).toBe(1);
        expect(decimals(1)).toBe(0);
        expect(decimals(10)).toBe(0);
        expect(decimals(undefined)).toBeUndefined();
        expect(formatValue(5.012, {type: 'number', resolution: 0.001})).toBe('5.012');
        expect(formatValue(5, {type: 'number', resolution: 0.01})).toBe('5.00');
        expect(formatValue(1 / 3, {type: 'number'})).toBe('0.333333');
        expect(formatValue(true, {type: 'bool'})).toBe('on');
        expect(formatValue(null, {type: 'number'})).toBe('—');
        expect(formatValue(NaN, {type: 'number'})).toBe('NaN');
    });

    it('step by their resolution, else a hundredth of their range', () => {
        expect(stepOf({resolution: 0.01})).toBe(0.01);
        expect(stepOf({min: 0, max: 500})).toBe(5);
        expect(stepOf({})).toBe(0.01);
    });
});
