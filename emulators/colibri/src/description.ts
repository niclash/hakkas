// The environment description the emulator gives hakkas (spec §6, 04 §1): the ports per slot,
// the modules and the carrier, built from the emulated system; and the checks hakkas makes of
// it (04 §0, "Validation"), so a description it would refuse is found here first.
import type {Json, JsonObject} from './check.js';
import type {Channel} from './descriptors.js';
import type {System} from './system.js';

// hakkas' limits (src/hakkas_env.h).
export const PORTS_MAX = 256;
export const MODULES_MAX = 64;
export const ID_MAX = 64;
export const TEXT_MAX = 1024;

export interface PortDescription {
    id: string;
    type: 'number' | 'bool';
    description?: string;
    unit?: string;
    min?: number;
    max?: number;
    addressable?: true;
    count?: number;
}

export interface ModuleDescription {
    slot: number;
    builtin?: true;
    type: string;
    name?: string;
    vendor?: string;
    revision?: string;
    serial?: string;
    documentation?: string;
    ports: string[];
}

export interface Description {
    name: string;
    description: string;
    icon: string;
    outPorts: PortDescription[];
    inPorts: PortDescription[];
    modules: ModuleDescription[];
    carrier: JsonObject;
}

export function describe(system: System): Description {
    const c = system.carrier;
    const d: Description = {
        name: system.name,
        description: `Colibri emulator: ${c.name ?? c.type}`,
        icon: 'chip',
        outPorts: [],
        inPorts: [],
        modules: [],
        carrier: {},
    };
    for (const slot of system.slots) {
        const module = slot.module;
        if (module === undefined)
            continue;
        const ports: string[] = [];
        for (const ch of module.channels) {
            const port = portOf(`${slot.part}-${ch.id}`, ch);
            (ch.direction === 'output' ? d.inPorts : d.outPorts).push(port);
            ports.push(port.id);
        }
        d.modules.push({
            slot: slot.index,
            ...(slot.builtin && {builtin: true}),
            type: module.type,
            ...optional({name: module.name, vendor: module.vendor, revision: module.revision,
                serial: slot.serial, documentation: module.documentation}),
            ports,
        });
    }
    d.carrier = {
        type: c.type,
        ...optional({name: c.name, vendor: c.vendor, revision: c.revision, documentation: c.documentation}),
        slots: c.slots,
        builtin: c.builtin.map(b => ({slot: b.slot})),
        ...(c.power !== undefined && {power: c.power}),
        comm: system.comm.map(s => ({
            slot: s.slot,
            type: s.def.type,
            ...optional({name: s.def.name}),
            ...(s.params as JsonObject),
        })),
    };
    return d;
}

function portOf(id: string, ch: Channel): PortDescription {
    return {
        id,
        type: ch.type,
        ...optional({description: ch.description, unit: ch.unit, min: ch.min, max: ch.max}),
        ...(ch.count > 1 && {addressable: true, count: ch.count}),
    };
}

// The members of `o` that are set.
function optional<T extends Record<string, Json | undefined>>(o: T): Partial<T> {
    return Object.fromEntries(Object.entries(o).filter(([, v]) => v !== undefined)) as Partial<T>;
}

// The problems hakkas would refuse the description for (04 §0): the first ones it would name,
// in its terms. A description the emulator builds from checked plugins can only break the
// count limits, but every rule is checked.
export function checkDescription(d: Description): string[] {
    const problems: string[] = [];
    const bytes = (s: string) => Buffer.byteLength(s, 'utf8');
    const text = (s: string | undefined) => s === undefined || bytes(s) <= TEXT_MAX;
    if (bytes(d.name) < 1 || bytes(d.name) > ID_MAX)
        problems.push(`name: expected 1 to ${ID_MAX} bytes`);
    if (!text(d.description) || !text(d.icon))
        problems.push(`text longer than ${TEXT_MAX} bytes`);
    const ports = [...d.outPorts, ...d.inPorts];
    if (ports.length > PORTS_MAX)
        problems.push(`${ports.length} ports, more than ${PORTS_MAX}`);
    const ids = new Set<string>();
    for (const p of ports) {
        if (bytes(p.id) < 1 || bytes(p.id) > ID_MAX)
            problems.push(`${p.id}: id: expected 1 to ${ID_MAX} bytes`);
        if (ids.has(p.id))
            problems.push(`duplicate port id "${p.id}"`);
        ids.add(p.id);
        if (p.count !== undefined && (p.count < 1 || p.count > 1024))
            problems.push(`${p.id}: count: expected 1 to 1024`);
        if (p.min !== undefined && p.max !== undefined && p.min > p.max)
            problems.push(`${p.id}: min greater than max`);
        if (!text(p.description) || !text(p.unit))
            problems.push(`${p.id}: text longer than ${TEXT_MAX} bytes`);
    }
    if (d.modules.length > MODULES_MAX)
        problems.push(`${d.modules.length} modules, more than ${MODULES_MAX}`);
    for (const m of d.modules)
        for (const s of [m.type, m.name, m.vendor, m.revision, m.serial, m.documentation])
            if (!text(s))
                problems.push(`slot ${m.slot}: text longer than ${TEXT_MAX} bytes`);
    return problems;
}
