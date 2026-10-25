// Evaluating sources (control protocol §5) and bringing values into a channel's range and
// resolution (spec §7.2). Times are in seconds since the source was set.
import type {NumberSource, Source} from './sources.js';

export function evaluate(source: Source, t: number): number | boolean {
    switch (source.kind) {
    case 'constant':
        return source.value;
    case 'sine':
        return source.offset + source.amplitude * Math.sin(2 * Math.PI * t / source.period);
    case 'square':
        return phase(t, source.period) < source.duty / 100 ? source.high : source.low;
    case 'ramp': {
        const f = source.repeat ? phase(t, source.period) : Math.min(t / source.period, 1);
        return source.from + (source.to - source.from) * f;
    }
    case 'counter':
        return Math.floor(t * source.rate);
    case 'sequence': {
        const total = source.steps.reduce((sum, [seconds]) => sum + seconds, 0);
        let at = source.repeat ? t % total : t;
        for (const [seconds, value] of source.steps) {
            if (at < seconds)
                return value;
            at -= seconds;
        }
        return source.steps.at(-1)![1];
    }
    case 'pulse':
        throw new Error('a pulse train is a signal: measure it');
    }
}

export function evaluateNumber(source: NumberSource, t: number): number {
    return evaluate(source, t) as number;
}

// Where in its period `t` is, from 0 to 1.
function phase(t: number, period: number): number {
    const p = (t % period) / period;
    return p < 0 ? p + 1 : p;
}

export interface Range {
    min?: number;
    max?: number;
    resolution?: number;
}

// value = round(clamp(v, min, max), resolution), and still within the range afterwards (a
// bound that is not a multiple of the resolution would otherwise be passed).
export function fit(v: number, range: Range): number {
    let x = clamp(v, range);
    if (range.resolution !== undefined)
        x = clamp(roundTo(x, range.resolution), range);
    return x;
}

function clamp(v: number, {min, max}: Range): number {
    return Math.min(Math.max(v, min ?? -Infinity), max ?? Infinity);
}

// Rounds to a multiple of `resolution`, without the binary fractions of a plain multiplication
// (5.012 rather than 5.0120000000000005).
export function roundTo(v: number, resolution: number): number {
    const r = Math.round(v / resolution) * resolution;
    return Number(r.toFixed(decimals(resolution)));
}

function decimals(resolution: number): number {
    if (Number.isInteger(resolution))
        return 0;
    const [mantissa, exponent] = resolution.toExponential().split('e');
    const fraction = mantissa!.split('.')[1]?.length ?? 0;
    return Math.min(Math.max(fraction - Number(exponent), 0), 15);
}

// The `n`th value of a count that starts at `min` and rolls over past `max` to `min` (spec §7.2:
// a DII's 32-bit counter).
export function rollOver(n: number, min = 0, max = Number.MAX_SAFE_INTEGER): number {
    const span = max - min + 1;
    const r = n % span;
    return min + (r < 0 ? r + span : r);
}
