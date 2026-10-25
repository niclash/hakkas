// Sources (01-control-protocol.md §5) as the source editor handles them: which kind one is, a
// sensible new one of a kind for a row, a short description, and formatting a row's value.
import type {Json, JsonObject, Row, SourceKind} from './protocol/types';

export const NUMBER_KINDS: SourceKind[] = ['constant', 'sine', 'square', 'ramp', 'sequence'];

export const KIND_LABELS: Record<SourceKind, string> = {
    constant: 'Constant', sine: 'Sine', square: 'Square', ramp: 'Ramp', counter: 'Counter',
    sequence: 'Sequence', pulse: 'Pulse train',
};

// The kind of a source, from its form.
export function kindOf(source: Json | undefined): SourceKind | undefined {
    if (typeof source === 'number' || typeof source === 'boolean')
        return 'constant';
    if (typeof source !== 'object' || source === null || Array.isArray(source))
        return undefined;
    const keys = Object.keys(source);
    return keys.length === 1 ? keys[0] as SourceKind : undefined;
}

// The parameters of a function source (its object's one member).
export function paramsOf(source: Json | undefined): JsonObject {
    const kind = kindOf(source);
    if (kind === undefined || kind === 'constant')
        return {};
    return (source as JsonObject)[kind] as JsonObject;
}

interface Range {
    type: 'number' | 'bool' | 'pulse';
    min?: number;
    max?: number;
}

// A new source of `kind` for a row: within its range, starting from what it has now.
export function newSource(kind: SourceKind, row: Range, current?: Json): Json {
    const lo = row.min ?? 0;
    const hi = row.max ?? (row.min !== undefined ? row.min + 10 : 10);
    const mid = lo + (hi - lo) / 2;
    const now = typeof current === 'number' ? current : mid;
    if (row.type === 'bool') {
        switch (kind) {
        case 'square':
            return {square: {low: false, high: true, period: 2, duty: 50}};
        case 'sequence':
            return {sequence: {steps: [[1, false], [1, true]], repeat: true}};
        default:
            return typeof current === 'boolean' ? current : false;
        }
    }
    switch (kind) {
    case 'constant':
        return now;
    case 'sine':
        return {sine: {offset: mid, amplitude: (hi - lo) / 2, period: 10}};
    case 'square':
        return {square: {low: lo, high: hi, period: 2, duty: 50}};
    case 'ramp':
        return {ramp: {from: lo, to: hi, period: 10, repeat: true}};
    case 'counter':
        return {counter: {rate: 1}};
    case 'sequence':
        return {sequence: {steps: [[1, lo], [1, hi]], repeat: true}};
    case 'pulse':
        return {pulse: {frequency: 1, duty: 50}};
    }
}

// A source in a few words, for a row whose source is a function.
export function describeSource(source: Json | undefined, unit = ''): string {
    const kind = kindOf(source);
    const u = unit === '' ? '' : ` ${unit}`;
    const p = paramsOf(source);
    switch (kind) {
    case undefined:
        return '';
    case 'constant':
        return `${String(source)}${typeof source === 'number' ? u : ''}`;
    case 'sine':
        return `sine ${p.offset} ± ${p.amplitude}${u}, ${p.period} s`;
    case 'square':
        return `square ${p.low}/${p.high}${typeof p.low === 'number' ? u : ''}, ${p.period} s, ${p.duty} %`;
    case 'ramp':
        return `ramp ${p.from} → ${p.to}${u}, ${p.period} s${p.repeat ? ', repeating' : ''}`;
    case 'counter':
        return `counter ${p.rate}/s`;
    case 'sequence':
        return `sequence of ${(p.steps as Json[]).length}${p.repeat ? ', repeating' : ''}`;
    case 'pulse':
        return `pulses ${describeSource(p.frequency, 'Hz')}, duty ${describeSource(p.duty, '%')}`;
    }
}

// The decimals a resolution shows (0.001 → 3).
export function decimals(resolution: number | undefined): number | undefined {
    if (resolution === undefined)
        return undefined;
    if (Number.isInteger(resolution))
        return 0;
    const [mantissa, exponent] = resolution.toExponential().split('e');
    const fraction = mantissa!.split('.')[1]?.length ?? 0;
    return Math.min(Math.max(fraction - Number(exponent), 0), 15);
}

// A row's value as shown: a number with its resolution's decimals, a bool as on/off.
export function formatValue(value: Json | undefined, row: Pick<Row, 'type' | 'resolution'>): string {
    if (value === undefined || value === null)
        return '—';
    if (typeof value === 'boolean')
        return value ? 'on' : 'off';
    if (typeof value === 'number') {
        if (Number.isNaN(value))
            return 'NaN';
        const d = decimals(row.resolution);
        return d === undefined ? String(Number(value.toPrecision(6))) : value.toFixed(d);
    }
    return String(value);
}

// A slider's step: the resolution, or a hundredth of the range.
export function stepOf(row: Pick<Row, 'min' | 'max' | 'resolution'>): number {
    if (row.resolution !== undefined)
        return row.resolution;
    if (row.min !== undefined && row.max !== undefined)
        return (row.max - row.min) / 100;
    return 0.01;
}
