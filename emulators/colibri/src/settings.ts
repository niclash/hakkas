// The settings form (control protocol §3.3, spec §8.3): the running environment's configuration
// as fields and values, and `configure` (§4.2) applied to it.
import type {Catalog} from './catalog.js';
import {STRING_MAX, type Json, type JsonObject} from './check.js';
import type {CarrierType, CommParam} from './descriptors.js';
import {carrierOf, CYCLE_MS_MAX, QUEUE_SIZE_MAX, type CommConfig, type Environment} from './environment.js';

export interface Option {
    value: Json;
    label: string;
}

export interface Field {
    id: string;
    label: string;
    type: 'enum' | 'number' | 'integer' | 'text' | 'bool' | 'list';
    group?: string;
    description?: string;
    readonly?: true;
    restart?: true;
    options?: Option[];
    min?: number;
    max?: number;
    unit?: string;
    pattern?: RegExp;           // a text's form; not sent
}

export interface Settings {
    fields: Field[];
    values: Record<string, Json>;
}

// A refused `configure`; its message is the protocol's.
export class SettingsError extends Error {}

const BITRATES = [1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200];
const EUI = /^[0-9A-Fa-f]{16}$/;

// The fields of a communication module's parameter groups (04 §1), relative to `comm<n>.`.
const PARAM_FIELDS: Record<CommParam, (Omit<Field, 'id'> & {key: string})[]> = {
    ip: [
        {key: 'ip.mode', label: 'IP address', type: 'enum',
            options: [{value: 'dhcp', label: 'DHCP'}, {value: 'static', label: 'Static'}]},
        {key: 'ip.address', label: 'Address', type: 'text'},
        {key: 'ip.prefix', label: 'Prefix length', type: 'integer', min: 0, max: 128},
        {key: 'ip.gateway', label: 'Gateway', type: 'text'},
        {key: 'ip.dns', label: 'DNS servers', type: 'list'},
        {key: 'ip.mac', label: 'MAC address', type: 'text'},
    ],
    wifi: [{key: 'wifi.ssid', label: 'SSID', type: 'text'}],
    rs485: [
        {key: 'rs485.bitrate', label: 'Bitrate', type: 'enum', unit: 'bit/s',
            options: BITRATES.map(b => ({value: b, label: String(b)}))},
        {key: 'rs485.parity', label: 'Parity', type: 'enum',
            options: ['none', 'even', 'odd'].map(p => ({value: p, label: p}))},
    ],
    lorawan: [
        {key: 'lorawan.appEui', label: 'App EUI', type: 'text', description: '16 hex digits', pattern: EUI},
        {key: 'lorawan.devEui', label: 'Dev EUI', type: 'text', description: '16 hex digits', pattern: EUI},
        {key: 'lorawan.appKeySet', label: 'App Key set', type: 'bool'},
    ],
};

const label = (def: {type: string, name?: string}) => def.name ?? def.type;

// The module types a changeable slot may take: every one but those that only live in a
// carrier's built-in slot.
function moduleOptions(catalog: Catalog): Option[] {
    const builtin = new Set([...catalog.carriers.values()].flatMap(c => c.def.builtin.map(b => b.type)));
    return [...catalog.modules.values()]
        .filter(m => !builtin.has(m.def.type))
        .map(m => ({value: m.def.type, label: label(m.def)}))
        .sort((a, b) => a.label.localeCompare(b.label));
}

export function settings(env: Environment, catalog: Catalog): Settings {
    const carrier = carrierOf(env, catalog)!;
    const fields: Field[] = [];
    const values: Record<string, Json> = {};
    const add = (f: Field, value: Json) => {
        fields.push(f);
        values[f.id] = value;
    };

    const carriers: Option[] = [...catalog.carriers.values()].map(c => ({value: c.def.type, label: label(c.def)}));
    if (typeof env.carrier !== 'string' && !catalog.carriers.has(env.carrier.type))
        carriers.push({value: env.carrier.type, label: `${label(env.carrier)} (copied from a device)`});
    add({id: 'carrier', label: 'Carrier board', type: 'enum', restart: true, options: carriers}, carrier.type);
    add({id: 'cycleMs', label: 'Cycle time', type: 'integer', unit: 'ms', min: 1, max: CYCLE_MS_MAX}, env.cycleMs);
    add({id: 'queueSize', label: 'Write queue size', type: 'integer', min: 1, max: QUEUE_SIZE_MAX}, env.queueSize);

    const modules = moduleOptions(catalog);
    for (let n = 0; n < carrier.slots + carrier.builtin.length; n++) {
        const builtin = carrier.builtin.find(b => b.slot === n);
        const group = 'Slots';
        if (builtin !== undefined) {
            const def = catalog.modules.get(builtin.type)?.def;
            add({id: `slot${n}.type`, label: `Slot ${n}`, group, type: 'enum', readonly: true,
                options: [{value: builtin.type, label: def !== undefined ? label(def) : builtin.type}]}, builtin.type);
            continue;
        }
        const m = env.modules.find(x => x.slot === n);
        const options = [{value: '', label: 'Empty'}, ...modules];
        if (m !== undefined && !options.some(o => o.value === m.type))
            options.push({value: m.type, label: `${m.type} (no plugin)`});
        add({id: `slot${n}.type`, label: `Slot ${n}`, group, type: 'enum', restart: true, options}, m?.type ?? '');
        if (m !== undefined)
            add({id: `slot${n}.serial`, label: `Slot ${n} serial number`, group, type: 'text', restart: true},
                m.serial ?? '');
    }

    for (const slot of carrier.comm) {
        const group = 'Communication';
        const prefix = `comm${slot.slot}`;
        const s = env.comm.find(x => x.slot === slot.slot);
        const options = [{value: '', label: 'Empty'},
            ...slot.types.filter(t => catalog.comm.has(t)).map(t => ({value: t, label: label(catalog.comm.get(t)!.def)}))];
        if (s !== undefined && !options.some(o => o.value === s.type))
            options.push({value: s.type, label: `${s.type} (no definition)`});
        add({id: `${prefix}.type`, label: `Communication slot ${slot.slot}`, group, type: 'enum', restart: true, options},
            s?.type ?? '');
        const def = s === undefined ? undefined : catalog.comm.get(s.type)?.def;
        for (const param of def?.params ?? [])
            for (const {key, ...f} of PARAM_FIELDS[param]) {
                const [g, member] = key.split('.') as [CommParam, string];
                add({...f, id: `${prefix}.${key}`, group}, s![g]?.[member] ?? null);
            }
    }
    return {fields, values};
}

// The environment with `values` applied (all or none), and whether that is a power cycle. The
// carrier and the slots' types come first, so their new fields can be set in the same request.
export function configure(env: Environment, catalog: Catalog,
                          values: Record<string, Json>): {env: Environment, restart: boolean} {
    const draft: Environment = structuredClone(env);
    let restart = false;
    const order = (id: string) => id === 'carrier' ? 0 : id.endsWith('.type') ? 1 : 2;
    const ids = Object.keys(values).sort((a, b) => order(a) - order(b));
    for (const id of ids) {
        const {fields, values: current} = settings(draft, catalog);
        const field = fields.find(f => f.id === id);
        if (field === undefined)
            throw new SettingsError(`Unknown setting: ${id}`);
        const value = values[id]!;
        if (JSON.stringify(value) === JSON.stringify(current[id]))
            continue;
        if (field.readonly)
            throw new SettingsError(`Read-only: ${id}`);
        if (!valid(field, value))
            throw new SettingsError(`Invalid value: ${id}`);
        apply(draft, catalog, id, value);
        if (field.restart)
            restart = true;
    }
    return {env: draft, restart};
}

function valid(f: Field, v: Json): boolean {
    const optional = f.id !== 'carrier' && f.id !== 'cycleMs' && f.id !== 'queueSize' && !f.id.endsWith('.type');
    if (v === null)
        return optional;
    switch (f.type) {
    case 'enum':
        return f.options!.some(o => o.value === v);
    case 'integer':
    case 'number':
        return typeof v === 'number' && Number.isFinite(v) && (f.type === 'number' || Number.isInteger(v))
            && (f.min === undefined || v >= f.min) && (f.max === undefined || v <= f.max);
    case 'text':
        return typeof v === 'string' && Buffer.byteLength(v) <= STRING_MAX
            && (v === '' || f.pattern === undefined || f.pattern.test(v));
    case 'bool':
        return typeof v === 'boolean';
    case 'list':
        return Array.isArray(v) && v.length <= 16
            && v.every(x => typeof x === 'string' && x !== '' && Buffer.byteLength(x) <= STRING_MAX);
    }
}

function apply(env: Environment, catalog: Catalog, id: string, v: Json): void {
    if (id === 'carrier')
        return setCarrier(env, catalog, v as string);
    if (id === 'cycleMs' || id === 'queueSize') {
        env[id] = v as number;
        return;
    }
    const [head, ...rest] = id.split('.');
    const n = Number(head!.replace(/^(slot|comm)/, ''));
    if (head!.startsWith('slot')) {
        const i = env.modules.findIndex(m => m.slot === n);
        if (rest[0] === 'type') {
            if (i >= 0)
                env.modules.splice(i, 1);     // another module: its serial is not this one's
            if (v !== '')
                env.modules.push({slot: n, type: v as string});
            env.modules.sort((a, b) => a.slot - b.slot);
        } else if (v === null || v === '')
            delete env.modules[i]!.serial;
        else
            env.modules[i]!.serial = v as string;
        return;
    }
    const i = env.comm.findIndex(s => s.slot === n);
    if (rest[0] === 'type') {
        const old = i >= 0 ? env.comm.splice(i, 1)[0] : undefined;
        if (v !== '') {
            // The parameter groups the new type has too are kept (Ethernet's address for WiFi).
            const params = catalog.comm.get(v as string)?.def.params ?? [];
            const s: CommConfig = {slot: n, type: v as string};
            for (const p of params)
                if (old?.[p] !== undefined)
                    s[p] = old[p];
            env.comm.push(s);
            env.comm.sort((a, b) => a.slot - b.slot);
        }
        return;
    }
    const s = env.comm[i]!;
    const [group, member] = rest as [CommParam, string];
    const o: JsonObject = s[group] ?? {};
    if (v === null || v === '' || (Array.isArray(v) && v.length === 0))
        delete o[member];
    else
        o[member] = v;
    if (Object.keys(o).length > 0)
        s[group] = o;
    else
        delete s[group];
}

// Another carrier: what it has no place for is dropped — modules in slots it lacks or has
// built in, communication modules in slots it lacks or that do not take them.
function setCarrier(env: Environment, catalog: Catalog, type: string): void {
    const carrier: CarrierType | undefined = catalog.carriers.get(type)?.def
        ?? (typeof env.carrier !== 'string' && env.carrier.type === type ? env.carrier : undefined);
    if (carrier === undefined)
        throw new SettingsError('Invalid value: carrier');
    env.carrier = catalog.carriers.has(type) ? type : carrier;
    const total = carrier.slots + carrier.builtin.length;
    env.modules = env.modules.filter(m => m.slot < total && !carrier.builtin.some(b => b.slot === m.slot));
    env.comm = env.comm.filter(s => carrier.comm.find(c => c.slot === s.slot)?.types.includes(s.type));
}

// A field as the protocol sends it (without what is only for checking).
export function fieldToJson({pattern: _, ...f}: Field): JsonObject {
    return f as unknown as JsonObject;
}
