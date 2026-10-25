// The descriptors of module types (spec §7.1), carrier types, communication module types and MCU
// module types (§5.4): what they hold, and reading one from its JSON form.
import {Checker, join, type Json, type JsonObject} from './check.js';
import {acceptedSources, readSource, type Source, type SourceKind} from './sources.js';

// A type name (spec §2.1): `colibri_pt1000`, `colibri_8`, `carrier`.
export const TYPE_NAME = /^[a-z0-9_]{1,64}$/;

// A channel's or signal's id (spec §7.1), so port ids (`slot3-ai`) and keys (`slot3.ai.1`) are
// well formed.
export const CHANNEL_ID = /^[a-z0-9_]{1,32}$/;

// The most channels of one group: an addressable port's count (hakkas' HAKKAS_ENV_COUNT_MAX).
export const COUNT_MAX = 1024;

// A picture file in the descriptor's directory: a plain file name, so serving it can never reach
// outside that directory.
export const ASSET_NAME = /^[A-Za-z0-9_][A-Za-z0-9._-]*\.(svg|png|jpg|jpeg|webp)$/;

export interface Metadata {
    type: string;
    name?: string;
    vendor?: string;
    revision?: string;
    description?: string;
    documentation?: string;
}

export type Quantity = 'state' | 'frequency' | 'duty' | 'count';
export type Control = 'slider' | 'number' | 'toggle' | 'button';
export type Indicator = 'lamp' | 'number' | 'bar' | 'meter';

interface ChannelBase {
    id: string;
    type: 'number' | 'bool';
    count: number;
    description?: string;
    unit?: string;
    min?: number;
    max?: number;
}

// A module input with a source of its own (an analog input).
export interface SourcedInput extends ChannelBase {
    direction: 'input';
    resolution?: number;
    default: Source;
    sources: SourceKind[];
    noise: number;
    control: Control;
}

// A module input measured from a signal (a DII terminal's frequency).
export interface MeasuredInput extends ChannelBase {
    direction: 'input';
    resolution?: number;
    measure: {signal: string, quantity: Quantity};
    noise: number;
}

export interface Output extends ChannelBase {
    direction: 'output';
    default: number | boolean;
    indicator: Indicator;
}

export type Channel = SourcedInput | MeasuredInput | Output;

export function isMeasured(c: Channel): c is MeasuredInput {
    return 'measure' in c;
}

// Whether a channel takes noise (spec §7.2): number inputs, but not a measured count, which
// counts pulses.
export function hasNoise(c: Channel): boolean {
    return c.direction === 'input' && c.type === 'number' && !(isMeasured(c) && c.measure.quantity === 'count');
}

export interface Signal {
    id: string;
    type: 'pulse';
    count: number;
    description?: string;
    default: Source;
}

// A binding or control on a picture (control protocol §6), with its key relative to the module.
export type PictureItem = JsonObject & {element: string, key: string};

export interface ModuleType extends Metadata {
    channels: Channel[];
    signals: Signal[];
    faceplate?: string;
    bindings: PictureItem[];
    controls: PictureItem[];
}

export interface CarrierType extends Metadata {
    slots: number;                              // the changeable I/O slots
    builtin: {slot: number, type: string}[];
    power?: JsonObject;
    comm: {slot: number, types: string[]}[];
    mcu?: {types: string[]};                    // the MCU module types it takes
    picture?: string;
}

export const COMM_PARAMS = ['ip', 'wifi', 'rs485', 'lorawan'] as const;
export type CommParam = typeof COMM_PARAMS[number];

export interface CommType extends Metadata {
    params: CommParam[];
}

// An MCU module: only named, not emulated (spec §2).
export type McuType = Metadata;

// hakkas' limits on a carrier (04 §0, "Validation").
export const IO_SLOTS_MAX = 31;
export const COMM_SLOTS_MAX = 4;

const METADATA = ['type', 'name', 'vendor', 'revision', 'description', 'documentation'];

function metadata(c: Checker, o: JsonObject, dirName: string): Metadata {
    const type = c.string(o, 'type', '', true);
    if (type !== undefined && !TYPE_NAME.test(type))
        c.error('type', 'expected 1 to 64 lower-case letters, digits and _');
    else if (type !== undefined && type !== dirName)
        c.error('type', `differs from its directory's name, ${dirName}`);
    const m: Metadata = {type: type ?? dirName};
    for (const key of ['name', 'vendor', 'revision', 'description', 'documentation'] as const) {
        const v = c.string(o, key, '');
        if (v !== undefined)
            m[key] = v;
    }
    return m;
}

function asset(c: Checker, o: JsonObject, key: string): string | undefined {
    const v = c.string(o, key, '');
    if (v !== undefined && !ASSET_NAME.test(v))
        return c.error(key, 'expected a picture file name in the same directory (.svg, .png, .jpg, .webp)');
    return v;
}

// A module type's descriptor, `module.json` in the directory `dirName`.
export function readModule(c: Checker, v: Json, dirName: string): ModuleType | undefined {
    const o = c.object(v, '');
    if (o === undefined)
        return undefined;
    c.members(o, [...METADATA, 'signals', 'channels', 'faceplate', 'ui'], '');
    const m: ModuleType = {...metadata(c, o, dirName), channels: [], signals: [], bindings: [], controls: []};
    const ids = new Set<string>();
    const unique = (id: string | undefined, path: string) => {
        if (id === undefined)
            return;
        if (ids.has(id))
            c.error(path, `${id} is defined twice`);
        ids.add(id);
    };

    const signals = o.signals === undefined ? [] : c.array(o.signals, 'signals') ?? [];
    signals.forEach((s, i) => {
        const sig = readSignal(c, s, join('signals', i));
        unique(sig?.id, join('signals', i));
        if (sig !== undefined)
            m.signals.push(sig);
    });
    const channels = o.channels === undefined ? [] : c.array(o.channels, 'channels') ?? [];
    channels.forEach((ch, i) => {
        const channel = readChannel(c, ch, join('channels', i), m.signals);
        unique(channel?.id, join('channels', i));
        if (channel !== undefined)
            m.channels.push(channel);
    });

    const faceplate = asset(c, o, 'faceplate');
    if (faceplate !== undefined)
        m.faceplate = faceplate;
    if (o.ui !== undefined) {
        const ui = c.object(o.ui, 'ui');
        if (ui !== undefined) {
            c.members(ui, ['bindings', 'controls'], 'ui');
            m.bindings = pictureItems(c, ui, 'bindings', m);
            m.controls = pictureItems(c, ui, 'controls', m);
        }
    }
    return c.ok ? m : undefined;
}

function readSignal(c: Checker, v: Json, path: string): Signal | undefined {
    const o = c.object(v, path);
    if (o === undefined)
        return undefined;
    c.members(o, ['id', 'type', 'count', 'description', 'default'], path);
    const errors = c.errors.length;
    const id = channelId(c, o, path);
    c.oneOf(o, 'type', path, ['pulse'] as const, true);
    const count = c.integer(o, 'count', path, 1, COUNT_MAX) ?? 1;
    const description = c.string(o, 'description', path);
    const dflt = o.default === undefined
        ? {kind: 'pulse' as const, frequency: {kind: 'constant' as const, value: 0}, duty: {kind: 'constant' as const, value: 0}}
        : readSource(c, o.default, join(path, 'default'), 'pulse', ['pulse']);
    if (c.errors.length !== errors)
        return undefined;
    return {id: id!, type: 'pulse', count, ...(description !== undefined && {description}), default: dflt!};
}

const QUANTITY_TYPES: Record<Quantity, 'number' | 'bool'> =
    {state: 'bool', frequency: 'number', duty: 'number', count: 'number'};

function readChannel(c: Checker, v: Json, path: string, signals: Signal[]): Channel | undefined {
    const o = c.object(v, path);
    if (o === undefined)
        return undefined;
    const errors = c.errors.length;
    const id = channelId(c, o, path);
    const direction = c.oneOf(o, 'direction', path, ['input', 'output'] as const, true);
    const type = c.oneOf(o, 'type', path, ['number', 'bool'] as const, true);
    const base: Partial<ChannelBase> = {count: c.integer(o, 'count', path, 1, COUNT_MAX) ?? 1};
    const description = c.string(o, 'description', path), unit = c.string(o, 'unit', path);
    if (description !== undefined)
        base.description = description;
    if (unit !== undefined)
        base.unit = unit;
    if (type === 'number') {
        const min = c.number(o, 'min', path), max = c.number(o, 'max', path);
        if (min !== undefined)
            base.min = min;
        if (max !== undefined)
            base.max = max;
        if (min !== undefined && max !== undefined && min > max)
            c.error(join(path, 'min'), 'greater than max');
    } else {
        for (const key of ['min', 'max', 'resolution', 'noise'])
            if (o[key] !== undefined)
                c.error(join(path, key), 'not for a bool channel');
    }
    if (id === undefined || direction === undefined || type === undefined || c.errors.length !== errors)
        return undefined;
    Object.assign(base, {id, type});

    if (direction === 'output') {
        c.members(o, ['id', 'direction', 'type', 'count', 'description', 'unit', 'min', 'max', 'default',
            'indicator'], path);
        for (const key of ['resolution', 'noise', 'control', 'measure'])
            if (o[key] !== undefined)
                c.error(join(path, key), 'not for an output');
        const dflt = type === 'bool' ? c.boolean(o, 'default', path) ?? false : c.number(o, 'default', path) ?? 0;
        const indicator = c.oneOf(o, 'indicator', path,
            type === 'bool' ? ['lamp'] as const : ['number', 'bar', 'meter'] as const)
            ?? (type === 'bool' ? 'lamp' : 'number');
        if ((indicator === 'bar' || indicator === 'meter') && (base.min === undefined || base.max === undefined))
            c.error(join(path, 'indicator'), `${indicator} needs min and max`);
        return c.errors.length === errors
            ? {...base as ChannelBase, direction, default: dflt, indicator}
            : undefined;
    }

    const resolution = type === 'number' ? c.number(o, 'resolution', path) : undefined;
    if (resolution !== undefined && resolution <= 0)
        c.error(join(path, 'resolution'), 'expected a number > 0');
    const noise = type === 'number' ? c.number(o, 'noise', path) ?? 0 : 0;
    if (noise < 0)
        c.error(join(path, 'noise'), 'expected a number ≥ 0');
    const extra = resolution !== undefined ? {resolution} : {};

    if (o.measure !== undefined) {
        c.members(o, ['id', 'direction', 'type', 'count', 'description', 'unit', 'min', 'max', 'resolution',
            'noise', 'measure'], path);
        for (const key of ['default', 'control'])
            if (o[key] !== undefined)
                c.error(join(path, key), 'not for a measured input');
        const mp = join(path, 'measure');
        const m = c.object(o.measure, mp);
        if (m === undefined)
            return undefined;
        c.members(m, ['signal', 'quantity'], mp);
        const signal = c.string(m, 'signal', mp, true);
        const quantity = c.oneOf(m, 'quantity', mp, ['state', 'frequency', 'duty', 'count'] as const, true);
        const sig = signals.find(s => s.id === signal);
        if (signal !== undefined && sig === undefined)
            c.error(join(mp, 'signal'), `no signal ${signal} (signals come before the channels measuring them)`);
        if (sig !== undefined && sig.count !== base.count)
            c.error(join(path, 'count'), `differs from signal ${sig.id}'s count, ${sig.count}`);
        if (quantity !== undefined && QUANTITY_TYPES[quantity] !== type)
            c.error(join(mp, 'quantity'), `${quantity} is a ${QUANTITY_TYPES[quantity]}`);
        if (quantity === 'count' && noise !== 0)
            c.error(join(path, 'noise'), 'not for a count');
        return c.errors.length === errors
            ? {...base as ChannelBase, direction, ...extra, measure: {signal: signal!, quantity: quantity!}, noise}
            : undefined;
    }

    c.members(o, ['id', 'direction', 'type', 'count', 'description', 'unit', 'min', 'max', 'resolution',
        'default', 'noise', 'control'], path);
    const sources = acceptedSources(type, resolution);
    // Without a default: false, or 0 brought into the channel's range.
    const zero = Math.min(Math.max(0, base.min ?? -Infinity), base.max ?? Infinity);
    const dflt = o.default === undefined
        ? {kind: 'constant' as const, value: type === 'bool' ? false : zero}
        : readSource(c, o.default, join(path, 'default'), type, sources);
    const control = c.oneOf(o, 'control', path,
        type === 'bool' ? ['toggle', 'button'] as const : ['slider', 'number'] as const)
        ?? (type === 'bool' ? 'toggle' : base.min !== undefined && base.max !== undefined ? 'slider' : 'number');
    if (control === 'slider' && (base.min === undefined || base.max === undefined))
        c.error(join(path, 'control'), 'a slider needs min and max');
    return c.errors.length === errors
        ? {...base as ChannelBase, direction, ...extra, default: dflt!, sources, noise, control}
        : undefined;
}

function channelId(c: Checker, o: JsonObject, path: string): string | undefined {
    const id = c.string(o, 'id', path, true);
    if (id !== undefined && !CHANNEL_ID.test(id))
        return c.error(join(path, 'id'), 'expected 1 to 32 lower-case letters, digits and _');
    return id;
}

// A picture's bindings or controls (control protocol §6), each with an `element` and a key
// relative to the module: `<id>[.<index>][.source|.noise]`, naming a channel or signal it has.
function pictureItems(c: Checker, ui: JsonObject, key: 'bindings' | 'controls', m: ModuleType): PictureItem[] {
    if (ui[key] === undefined)
        return [];
    const path = join('ui', key);
    const list = c.array(ui[key], path) ?? [];
    const out: PictureItem[] = [];
    list.forEach((v, i) => {
        const p = join(path, i);
        const o = c.object(v, p);
        if (o === undefined)
            return;
        const element = c.string(o, 'element', p, true);
        const keyMember = key === 'controls' ? 'key' : ['key', 'text', 'visible'].find(k => o[k] !== undefined);
        if (keyMember === undefined)
            return c.error(p, 'expected key, text or visible');
        const k = c.string(o, keyMember, p, true);
        if (key === 'controls') {
            c.oneOf(o, 'control', p, ['slider', 'number', 'toggle', 'button'] as const, true);
            if (k !== undefined && !k.endsWith('.source'))
                c.error(join(p, 'key'), 'a control sets a source: expected a key ending in .source');
        }
        if (k !== undefined && !relativeKeyExists(k, m))
            c.error(join(p, keyMember), `no such key in this module: ${k}`);
        if (element !== undefined && k !== undefined)
            out.push({...o, element, key: k});
    });
    return out;
}

function relativeKeyExists(key: string, m: ModuleType): boolean {
    const parts = key.split('.');
    const attribute = parts.at(-1) === 'source' || parts.at(-1) === 'noise' ? parts.pop() : undefined;
    const [id, index, ...rest] = parts;
    if (rest.length > 0)
        return false;
    const group: {count: number} | undefined =
        m.channels.find(ch => ch.id === id) ?? m.signals.find(s => s.id === id);
    if (group === undefined)
        return false;
    if (group.count === 1 ? index !== undefined : !(index !== undefined && /^[1-9][0-9]*$/.test(index)
        && Number(index) <= group.count))
        return false;
    const channel = m.channels.find(ch => ch.id === id);
    if (attribute === 'source')
        return channel === undefined || (channel.direction === 'input' && !isMeasured(channel));
    if (attribute === 'noise')
        return channel !== undefined && hasNoise(channel);
    return true;
}

// A carrier type's descriptor, `carrier.json` (spec §5.4). The module types of its built-in
// slots and its communication types are checked against the catalog by the loader.
export function readCarrier(c: Checker, v: Json, dirName: string): CarrierType | undefined {
    const o = c.object(v, '');
    if (o === undefined)
        return undefined;
    c.members(o, [...METADATA, 'slots', 'builtin', 'power', 'comm', 'mcu', 'picture'], '');
    const slots = c.integer(o, 'slots', '', 0, IO_SLOTS_MAX, true) ?? 0;
    const carrier: CarrierType = {...metadata(c, o, dirName), slots, builtin: [], comm: []};

    const builtin = o.builtin === undefined ? [] : c.array(o.builtin, 'builtin') ?? [];
    const total = slots + builtin.length;
    if (total < 1 || total > IO_SLOTS_MAX)
        c.error('slots', `expected 1 to ${IO_SLOTS_MAX} I/O slots, built-in ones included`);
    builtin.forEach((b, i) => {
        const p = join('builtin', i);
        const e = c.object(b, p);
        if (e === undefined)
            return;
        c.members(e, ['slot', 'type'], p);
        const slot = c.integer(e, 'slot', p, 0, Math.max(0, total - 1), true);
        const type = c.string(e, 'type', p, true);
        if (slot !== undefined && carrier.builtin.some(x => x.slot === slot))
            c.error(join(p, 'slot'), `slot ${slot} is built in twice`);
        if (slot !== undefined && type !== undefined)
            carrier.builtin.push({slot, type});
    });

    if (o.power !== undefined) {
        const power = c.object(o.power, 'power');
        if (power !== undefined) {
            c.members(power, ['voltage', 'ac', 'dc', 'current'], 'power');
            if (power.voltage !== undefined) {
                const voltage = c.object(power.voltage, 'power.voltage');
                if (voltage !== undefined) {
                    c.members(voltage, ['min', 'max'], 'power.voltage');
                    c.number(voltage, 'min', 'power.voltage');
                    c.number(voltage, 'max', 'power.voltage');
                }
            }
            c.boolean(power, 'ac', 'power');
            c.boolean(power, 'dc', 'power');
            c.number(power, 'current', 'power');
            carrier.power = power;
        }
    }

    const comm = o.comm === undefined ? [] : c.array(o.comm, 'comm') ?? [];
    if (comm.length > COMM_SLOTS_MAX)
        c.error('comm', `more than ${COMM_SLOTS_MAX} communication slots`);
    comm.forEach((s, i) => {
        const p = join('comm', i);
        const e = c.object(s, p);
        if (e === undefined)
            return;
        c.members(e, ['slot', 'types'], p);
        const slot = c.integer(e, 'slot', p, 1, COMM_SLOTS_MAX, true);
        if (slot !== undefined && carrier.comm.some(x => x.slot === slot))
            c.error(join(p, 'slot'), `communication slot ${slot} is listed twice`);
        const types = c.array(e.types ?? null, join(p, 'types')) ?? [];
        if (types.length === 0)
            c.error(join(p, 'types'), 'expected at least one communication module type');
        const names: string[] = [];
        types.forEach((t, j) => {
            if (typeof t !== 'string' || !TYPE_NAME.test(t))
                return c.error(join(join(p, 'types'), j), 'expected a type name');
            names.push(t);
        });
        if (slot !== undefined)
            carrier.comm.push({slot, types: names});
    });

    if (o.mcu !== undefined) {
        const mcu = c.object(o.mcu, 'mcu');
        if (mcu !== undefined) {
            c.members(mcu, ['types'], 'mcu');
            const types = c.array(mcu.types ?? null, 'mcu.types') ?? [];
            if (types.length === 0)
                c.error('mcu.types', 'expected at least one MCU module type');
            const names: string[] = [];
            types.forEach((t, j) => {
                if (typeof t !== 'string' || !TYPE_NAME.test(t))
                    return c.error(join('mcu.types', j), 'expected a type name');
                names.push(t);
            });
            carrier.mcu = {types: names};
        }
    }

    const picture = asset(c, o, 'picture');
    if (picture !== undefined)
        carrier.picture = picture;
    return c.ok ? carrier : undefined;
}

// An MCU module type's descriptor, `mcu.json` (spec §5.4): its metadata.
export function readMcu(c: Checker, v: Json, dirName: string): McuType | undefined {
    const o = c.object(v, '');
    if (o === undefined)
        return undefined;
    c.members(o, METADATA, '');
    const mcu = metadata(c, o, dirName);
    return c.ok ? mcu : undefined;
}

// A communication module type's descriptor, `comm.json` (spec §5.4).
export function readComm(c: Checker, v: Json, dirName: string): CommType | undefined {
    const o = c.object(v, '');
    if (o === undefined)
        return undefined;
    c.members(o, [...METADATA, 'params'], '');
    const comm: CommType = {...metadata(c, o, dirName), params: []};
    const params = c.array(o.params ?? null, 'params') ?? [];
    params.forEach((p, i) => {
        if (typeof p !== 'string' || !(COMM_PARAMS as readonly string[]).includes(p))
            return c.error(join('params', i), `expected one of ${COMM_PARAMS.join(', ')}`);
        if (comm.params.includes(p as CommParam))
            return c.error(join('params', i), `${p} is listed twice`);
        comm.params.push(p as CommParam);
    });
    return c.ok ? comm : undefined;
}
