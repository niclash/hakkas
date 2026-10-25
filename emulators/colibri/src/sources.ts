// An input's or signal's source (control protocol §5): a constant or a function of time. This
// file reads and checks them; evaluating them is the model's (plan task E4).
import {Checker, isObject, join, type Json} from './check.js';

export type SourceKind = 'constant' | 'sine' | 'square' | 'ramp' | 'counter' | 'sequence' | 'pulse';
export const SOURCE_KINDS: readonly SourceKind[] =
    ['constant', 'sine', 'square', 'ramp', 'counter', 'sequence', 'pulse'];

// The type of what a source drives: a number or bool channel, or a pulse signal.
export type RowType = 'number' | 'bool' | 'pulse';

// A number source that a pulse train's frequency or duty cycle may follow.
export type NumberSource =
    | {kind: 'constant', value: number}
    | {kind: 'sine', offset: number, amplitude: number, period: number}
    | {kind: 'square', low: number, high: number, period: number, duty: number}
    | {kind: 'ramp', from: number, to: number, period: number, repeat: boolean}
    | {kind: 'sequence', steps: [number, number][], repeat: boolean};

export type Source =
    | NumberSource
    | {kind: 'constant', value: boolean}
    | {kind: 'square', low: boolean, high: boolean, period: number, duty: number}
    | {kind: 'sequence', steps: [number, boolean][], repeat: boolean}
    | {kind: 'counter', rate: number}
    | {kind: 'pulse', frequency: NumberSource, duty: NumberSource};

// The source kinds a row accepts (spec §8.3): number inputs every function but `pulse`, and
// `counter` only when they count in whole units; bool inputs constants, `square` and
// `sequence`; signals `pulse`.
export function acceptedSources(type: RowType, resolution?: number): SourceKind[] {
    switch (type) {
    case 'number':
        return resolution === 1
            ? ['constant', 'sine', 'square', 'ramp', 'counter', 'sequence']
            : ['constant', 'sine', 'square', 'ramp', 'sequence'];
    case 'bool':
        return ['constant', 'square', 'sequence'];
    case 'pulse':
        return ['pulse'];
    }
}

// Reads `v` as a source for a row of `type` that accepts `accepted`; problems go to `c`.
export function readSource(c: Checker, v: Json | undefined, path: string, type: RowType,
                           accepted: readonly SourceKind[]): Source | undefined {
    const kind = sourceKind(v);
    if (kind === undefined)
        return c.error(path, 'expected a source: a constant or an object with one member');
    if (!accepted.includes(kind))
        return c.error(path, `source ${kind} not accepted here (accepted: ${accepted.join(', ')})`);
    if (kind === 'constant') {
        if (type === 'number' && typeof v === 'number' && Number.isFinite(v))
            return {kind, value: v};
        if (type === 'bool' && typeof v === 'boolean')
            return {kind, value: v};
        return c.error(path, type === 'bool' ? 'expected true or false' : 'expected a number');
    }
    const p = join(path, kind);
    const o = c.object((v as {[k: string]: Json})[kind], p);
    if (o === undefined)
        return undefined;
    const errors = c.errors.length;
    let s: Source | undefined;
    switch (kind) {
    case 'sine':
        c.members(o, ['offset', 'amplitude', 'period'], p);
        s = {kind, offset: c.number(o, 'offset', p, true)!, amplitude: c.number(o, 'amplitude', p, true)!,
            period: period(c, o, p)!};
        break;
    case 'square': {
        c.members(o, ['low', 'high', 'period', 'duty'], p);
        const per = period(c, o, p)!, duty = percent(c, o, 'duty', p)!;
        s = type === 'bool'
            ? {kind, low: c.boolean(o, 'low', p) ?? false, high: c.boolean(o, 'high', p) ?? true, period: per, duty}
            : {kind, low: c.number(o, 'low', p, true)!, high: c.number(o, 'high', p, true)!, period: per, duty};
        break;
    }
    case 'ramp':
        c.members(o, ['from', 'to', 'period', 'repeat'], p);
        s = {kind, from: c.number(o, 'from', p, true)!, to: c.number(o, 'to', p, true)!, period: period(c, o, p)!,
            repeat: c.boolean(o, 'repeat', p) ?? false};
        break;
    case 'counter': {
        c.members(o, ['rate'], p);
        const rate = c.number(o, 'rate', p, true);
        if (rate !== undefined && rate < 0)
            c.error(join(p, 'rate'), 'expected a number ≥ 0');
        s = {kind, rate: rate!};
        break;
    }
    case 'sequence':
        c.members(o, ['steps', 'repeat'], p);
        s = {kind, steps: steps(c, o, p, type) as never, repeat: c.boolean(o, 'repeat', p) ?? false};
        break;
    case 'pulse': {
        c.members(o, ['frequency', 'duty'], p);
        const numeric: SourceKind[] = ['constant', 'sine', 'square', 'ramp', 'sequence'];
        const frequency = readSource(c, o.frequency, join(p, 'frequency'), 'number', numeric) as NumberSource;
        const duty = readSource(c, o.duty, join(p, 'duty'), 'number', numeric) as NumberSource;
        if (frequency?.kind === 'constant' && frequency.value < 0)
            c.error(join(p, 'frequency'), 'expected a number ≥ 0');
        if (duty?.kind === 'constant' && (duty.value < 0 || duty.value > 100))
            c.error(join(p, 'duty'), 'expected a number from 0 to 100');
        s = {kind, frequency, duty};
        break;
    }
    }
    return c.errors.length === errors ? s : undefined;
}

// The kind of source `v` is, from its form alone, or undefined when it has no source's form.
function sourceKind(v: Json | undefined): SourceKind | undefined {
    if (typeof v === 'number' || typeof v === 'boolean')
        return 'constant';
    if (!isObject(v))
        return undefined;
    const keys = Object.keys(v);
    if (keys.length !== 1)
        return undefined;
    const k = keys[0] as SourceKind;
    return SOURCE_KINDS.includes(k) && k !== 'constant' ? k : undefined;
}

function period(c: Checker, o: {[k: string]: Json}, path: string): number | undefined {
    const v = c.number(o, 'period', path, true);
    if (v !== undefined && v <= 0)
        return c.error(join(path, 'period'), 'expected a number > 0');
    return v;
}

function percent(c: Checker, o: {[k: string]: Json}, key: string, path: string): number | undefined {
    const v = c.number(o, key, path, true);
    if (v !== undefined && (v < 0 || v > 100))
        return c.error(join(path, key), 'expected a number from 0 to 100');
    return v;
}

function steps(c: Checker, o: {[k: string]: Json}, path: string, type: RowType): [number, number | boolean][] {
    const p = join(path, 'steps');
    const list = o.steps === undefined ? c.error(p, 'missing') : c.array(o.steps, p);
    if (list === undefined)
        return [];
    if (list.length === 0)
        c.error(p, 'expected at least one step');
    const out: [number, number | boolean][] = [];
    list.forEach((step, i) => {
        const sp = join(p, i);
        if (!Array.isArray(step) || step.length !== 2)
            return c.error(sp, 'expected [seconds, value]');
        const [seconds, value] = step;
        if (typeof seconds !== 'number' || !Number.isFinite(seconds) || seconds <= 0)
            return c.error(sp, 'expected a duration > 0 in seconds');
        if (type === 'bool' ? typeof value !== 'boolean' : typeof value !== 'number' || !Number.isFinite(value))
            return c.error(sp, type === 'bool' ? 'expected a true or false value' : 'expected a number value');
        out.push([seconds, value as number | boolean]);
    });
    return out;
}
