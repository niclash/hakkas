// The environment description the emulator gives hakkas (spec §6, 04 §1): the ports per slot,
// the modules and the carrier, built from the emulated system; and the checks hakkas makes of
// it (04 §0, "Validation"), so a description it would refuse is found here first.
import type {Json, JsonObject} from './check.js';
import type {Channel, McuType, Places} from './descriptors.js';
import type {System} from './system.js';

// hakkas' limits (src/hakkas_env.h).
export const PORTS_MAX = 256;
export const POSITIONS_MAX = 1024;
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
    picture?: string;
    ports: string[];
    positions?: {port: string, index?: number, x: number, y: number}[];
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

// `mcu` is the MCU module in the carrier's MCU slot, if it takes it (spec §5.4).
export function describe(system: System, mcu?: McuType): Description {
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
        const positions = module.positions.map(p => ({port: `${slot.part}-${p.channel}`,
            ...(p.index !== undefined && {index: p.index}), x: p.x, y: p.y}));
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
                serial: slot.serial, documentation: module.documentation, picture: module.pictureUrl}),
            ports,
            ...(positions.length > 0 && {positions}),
        });
    }
    d.carrier = {
        type: c.type,
        ...optional({name: c.name, vendor: c.vendor, revision: c.revision, documentation: c.documentation,
            picture: c.pictureUrl}),
        slots: c.slots,
        builtin: c.builtin.map(b => ({slot: b.slot})),
        ...(c.power !== undefined && {power: c.power}),
        comm: system.comm.map(s => ({
            slot: s.slot,
            type: s.def.type,
            ...optional({name: s.def.name, picture: s.def.pictureUrl}),
            ...(s.params as JsonObject),
        })),
        ...(mcu !== undefined && {mcu: {type: mcu.type, ...optional({name: mcu.name, vendor: mcu.vendor,
            revision: mcu.revision, documentation: mcu.documentation, picture: mcu.pictureUrl})}}),
        ...(c.places !== undefined && {places: placesOf(c.places, mcu !== undefined)}),
    };
    return d;
}

// The carrier's places in hakkas' form (04 §1); the MCU slot's only with an MCU module in it.
function placesOf(p: Places, mcu: boolean): JsonObject {
    return {
        ...(p.slots.length > 0 && {slots: p.slots.map(s => ({...s}))}),
        ...(p.comm.length > 0 && {comm: p.comm.map(s => ({...s}))}),
        ...(mcu && p.mcu !== undefined && {mcu: {...p.mcu}}),
    };
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
    const positions = d.modules.reduce((n, m) => n + (m.positions?.length ?? 0), 0);
    if (positions > POSITIONS_MAX)
        problems.push(`${positions} positions, more than ${POSITIONS_MAX}`);
    for (const m of d.modules)
        for (const s of [m.type, m.name, m.vendor, m.revision, m.serial, m.documentation, m.picture])
            if (!text(s))
                problems.push(`slot ${m.slot}: text longer than ${TEXT_MAX} bytes`);
    return problems;
}
