// An environment file, `<name>.json` (spec §5): reading its form, checking it against the
// catalog, and writing it back.
import type {Catalog} from './catalog.js';
import {Checker, isObject, join, type Json, type JsonObject} from './check.js';
import {COMM_SLOTS_MAX, IO_SLOTS_MAX, readCarrier, TYPE_NAME, type CarrierType, type CommParam} from './descriptors.js';

export const DEFAULT_CYCLE_MS = 10;
export const DEFAULT_QUEUE_SIZE = 10;
export const CYCLE_MS_MAX = 1000;
export const QUEUE_SIZE_MAX = 1000;
export const DEFAULT_CARRIER = 'colibri_8';

export interface ModuleConfig {
    slot: number;
    type: string;
    serial?: string;
}

// A communication slot's module and its view-only parameters (04 §1), as the file has them.
export interface CommConfig {
    slot: number;
    type: string;
    ip?: JsonObject;
    wifi?: JsonObject;
    rs485?: JsonObject;
    lorawan?: JsonObject;
}

// An input's or signal's source and noise, by key (spec §8.3), as the file has them; they are
// checked against the module when the emulated system is built (plan task E4).
export interface InputConfig {
    source?: Json;
    noise?: Json;
}

export interface Environment {
    carrier: string | CarrierType;     // a type, or a whole definition copied from a device (§5.3)
    cycleMs: number;
    queueSize: number;
    comm: CommConfig[];
    modules: ModuleConfig[];           // the changeable slots only
    inputs: Record<string, InputConfig>;
    extra: JsonObject;                 // members the emulator does not know, written back as they were
}

export function newEnvironment(): Environment {
    return {carrier: DEFAULT_CARRIER, cycleMs: DEFAULT_CYCLE_MS, queueSize: DEFAULT_QUEUE_SIZE,
        comm: [], modules: [], inputs: {}, extra: {}};
}

const MEMBERS = ['carrier', 'cycleMs', 'queueSize', 'comm', 'modules', 'inputs'];
const PARAMS: CommParam[] = ['ip', 'wifi', 'rs485', 'lorawan'];

// Reads an environment file's form. Problems that make it unusable are errors; its relation to
// the catalog is checkEnvironment's.
export function readEnvironment(c: Checker, v: Json): Environment | undefined {
    const o = c.object(v, '');
    if (o === undefined)
        return undefined;
    const env = newEnvironment();
    for (const [key, value] of Object.entries(o))
        if (!MEMBERS.includes(key))
            env.extra[key] = value;

    if (typeof o.carrier === 'string')
        env.carrier = o.carrier;
    else if (isObject(o.carrier)) {
        const sub = new Checker();
        const type = typeof o.carrier.type === 'string' ? o.carrier.type : '';
        const def = readCarrier(sub, o.carrier, type);
        for (const e of sub.errors)
            c.error('carrier', e);
        for (const w of sub.warnings)
            c.warn('carrier', w);
        if (def !== undefined)
            env.carrier = def;
    } else if (o.carrier !== undefined)
        c.error('carrier', 'expected a carrier type or a carrier definition');
    else
        c.error('carrier', 'missing');

    env.cycleMs = c.integer(o, 'cycleMs', '', 1, CYCLE_MS_MAX) ?? DEFAULT_CYCLE_MS;
    env.queueSize = c.integer(o, 'queueSize', '', 1, QUEUE_SIZE_MAX) ?? DEFAULT_QUEUE_SIZE;

    const modules = o.modules === undefined ? [] : c.array(o.modules, 'modules') ?? [];
    modules.forEach((m, i) => {
        const p = join('modules', i);
        const e = c.object(m, p);
        if (e === undefined)
            return;
        c.members(e, ['slot', 'type', 'serial'], p);
        const slot = c.integer(e, 'slot', p, 0, IO_SLOTS_MAX - 1, true);
        const type = typeName(c, e, p);
        const serial = c.string(e, 'serial', p);
        if (slot !== undefined && env.modules.some(x => x.slot === slot))
            c.error(join(p, 'slot'), `slot ${slot} is listed twice`);
        if (slot !== undefined && type !== undefined)
            env.modules.push({slot, type, ...(serial !== undefined && {serial})});
    });

    const comm = o.comm === undefined ? [] : c.array(o.comm, 'comm') ?? [];
    comm.forEach((s, i) => {
        const p = join('comm', i);
        const e = c.object(s, p);
        if (e === undefined)
            return;
        c.members(e, ['slot', 'type', ...PARAMS], p);
        const slot = c.integer(e, 'slot', p, 1, COMM_SLOTS_MAX, true);
        const type = typeName(c, e, p);
        if (slot !== undefined && env.comm.some(x => x.slot === slot))
            c.error(join(p, 'slot'), `communication slot ${slot} is listed twice`);
        const config: CommConfig = {slot: slot!, type: type!};
        for (const param of PARAMS)
            if (e[param] !== undefined) {
                const group = commParam(c, e[param], join(p, param), param);
                if (group !== undefined)
                    config[param] = group;
            }
        if (slot !== undefined && type !== undefined)
            env.comm.push(config);
    });

    if (o.inputs !== undefined) {
        const inputs = c.object(o.inputs, 'inputs');
        for (const [key, value] of Object.entries(inputs ?? {})) {
            const p = join('inputs', key);
            const e = c.object(value, p);
            if (e === undefined)
                continue;
            c.members(e, ['source', 'noise'], p);
            env.inputs[key] = {...(e.source !== undefined && {source: e.source}),
                ...(e.noise !== undefined && {noise: e.noise})};
        }
    }
    return c.ok ? env : undefined;
}

function typeName(c: Checker, o: JsonObject, path: string): string | undefined {
    const type = c.string(o, 'type', path, true);
    if (type !== undefined && !TYPE_NAME.test(type))
        return c.error(join(path, 'type'), 'expected a type name');
    return type;
}

// A communication parameter group (04 §1): view-only values, so only their JSON types are checked.
function commParam(c: Checker, v: Json, path: string, param: CommParam): JsonObject | undefined {
    const o = c.object(v, path);
    if (o === undefined)
        return undefined;
    switch (param) {
    case 'ip':
        c.members(o, ['mode', 'address', 'prefix', 'gateway', 'dns', 'mac'], path);
        c.oneOf(o, 'mode', path, ['dhcp', 'static'] as const);
        c.string(o, 'address', path);
        c.integer(o, 'prefix', path, 0, 128);
        c.string(o, 'gateway', path);
        c.string(o, 'mac', path);
        if (o.dns !== undefined && !(Array.isArray(o.dns) && o.dns.every(d => typeof d === 'string')))
            c.error(join(path, 'dns'), 'expected a list of addresses');
        break;
    case 'wifi':
        c.members(o, ['ssid'], path);
        c.string(o, 'ssid', path);
        break;
    case 'rs485':
        c.members(o, ['bitrate', 'parity'], path);
        c.integer(o, 'bitrate', path, 1, 10_000_000);
        c.oneOf(o, 'parity', path, ['none', 'even', 'odd'] as const);
        break;
    case 'lorawan':
        c.members(o, ['appEui', 'devEui', 'appKeySet'], path);
        c.string(o, 'appEui', path);
        c.string(o, 'devEui', path);
        c.boolean(o, 'appKeySet', path);
        break;
    }
    return o;
}

// The carrier definition an environment runs on, or undefined when the catalog lacks its type.
export function carrierOf(env: Environment, catalog: Catalog): CarrierType | undefined {
    return typeof env.carrier === 'string' ? catalog.carriers.get(env.carrier)?.def : env.carrier;
}

// Checks an environment against the catalog (spec §5). An error means it cannot be run: an
// unknown carrier, a slot or communication slot the carrier does not have, a module in a
// built-in slot. A module or communication module type the catalog lacks, or one its slot does
// not take, is a warning: the slot stays empty, and the file keeps it.
export function checkEnvironment(env: Environment, catalog: Catalog): {errors: string[], warnings: string[]} {
    const errors: string[] = [], warnings: string[] = [];
    const carrier = carrierOf(env, catalog);
    if (carrier === undefined) {
        errors.push(`unknown carrier type ${env.carrier as string}`);
        return {errors, warnings};
    }
    const total = carrier.slots + carrier.builtin.length;
    for (const b of carrier.builtin)
        if (!catalog.modules.has(b.type))
            warnings.push(`built-in slot ${b.slot}: unknown module type ${b.type}, the slot stays empty`);
    for (const m of env.modules) {
        if (m.slot >= total)
            errors.push(`slot ${m.slot}: the carrier has slots 0 to ${total - 1}`);
        else if (carrier.builtin.some(b => b.slot === m.slot))
            errors.push(`slot ${m.slot}: a built-in slot, its module cannot be changed`);
        else if (!catalog.modules.has(m.type))
            warnings.push(`slot ${m.slot}: unknown module type ${m.type}, the slot stays empty`);
    }
    for (const s of env.comm) {
        const slot = carrier.comm.find(x => x.slot === s.slot);
        const comm = catalog.comm.get(s.type);
        if (slot === undefined)
            errors.push(`communication slot ${s.slot}: the carrier has no such slot`);
        else if (comm === undefined)
            warnings.push(`communication slot ${s.slot}: unknown communication module type ${s.type}, the slot stays empty`);
        else if (!slot.types.includes(s.type))
            warnings.push(`communication slot ${s.slot}: does not take ${s.type}, the slot stays empty`);
        else
            for (const param of PARAMS)
                if (s[param] !== undefined && !comm.def.params.includes(param))
                    warnings.push(`communication slot ${s.slot}: ${s.type} has no ${param} parameters, ignored`);
    }
    return {errors, warnings};
}

// The file's JSON text: the known members in a fixed order, then the unknown ones as they were.
export function writeEnvironment(env: Environment): string {
    const o: JsonObject = {
        carrier: env.carrier as Json,
        cycleMs: env.cycleMs,
        queueSize: env.queueSize,
        comm: env.comm as unknown as Json,
        modules: env.modules as unknown as Json,
        inputs: env.inputs as unknown as Json,
    };
    for (const [key, value] of Object.entries(env.extra))
        if (!(key in o))
            o[key] = value;
    return JSON.stringify(o, null, 2) + '\n';
}
