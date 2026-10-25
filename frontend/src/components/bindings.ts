// Environment bindings in the frontend (06 §4.14): the environment's channels, what binding a
// block port to one would give, a bindings file's form (04-drivers.md §4) and its mapping.
// Kept free of Vue so that it can be unit tested. The checks follow the engine's own, in its
// order (04 §4, 02 §3.7), so the mapping dialog warns about exactly what the engine would skip;
// the engine stays the judge (a refusal is shown as usual).

import type {Binding, EnvDescription, EnvPort} from "@/components/protocols/environment";

// One channel of an environment port: index 1..count of an addressable port, none otherwise.
export interface Channel {
    env: string;
    index?: number;
    sensor: boolean;        // an environment output port: binds a block input
    label: string;          // "ain[2]", or "temp"
    type: string;
    unit?: string;
}

// A bindings file's entry, and a binding without its process.
export interface Entry {
    block: string;
    port: string;
    env: string;
    index?: number;
}

export function channelLabel(env: string, index?: number): string {
    return index !== undefined && index > 0 ? `${env}[${index}]` : env;
}

// The channel a label names ("ain[2]" or "temp").
export function parseChannel(label: string): { env: string, index?: number } {
    const m = /^(.*)\[(\d+)]$/.exec(label);
    return m === null ? {env: label} : {env: m[1], index: Number(m[2])};
}

function portChannels(p: EnvPort, sensor: boolean): Channel[] {
    const one = (index?: number): Channel => ({env: p.id, index, sensor, label: channelLabel(p.id, index), type: p.type, unit: p.unit});
    if (!p.addressable)
        return [one()];
    return Array.from({length: p.count ?? 0}, (_, i) => one(i + 1));
}

// Every channel: the sensors, then the actuators, each port's in order.
export function channelsOf(d: EnvDescription): Channel[] {
    return [...d.outPorts.flatMap(p => portChannels(p, true)), ...d.inPorts.flatMap(p => portChannels(p, false))];
}

// The channels of one port of the description.
export function channelsOfPort(d: EnvDescription, id: string): Channel[] {
    const out = d.outPorts.find(p => p.id === id);
    if (out !== undefined)
        return portChannels(out, true);
    const inp = d.inPorts.find(p => p.id === id);
    return inp !== undefined ? portChannels(inp, false) : [];
}

function findPort(d: EnvDescription, id: string): { port: EnvPort, sensor: boolean } | undefined {
    const out = d.outPorts.find(p => p.id === id);
    if (out !== undefined)
        return {port: out, sensor: true};
    const inp = d.inPorts.find(p => p.id === id);
    return inp !== undefined ? {port: inp, sensor: false} : undefined;
}

// Why the channel is not one of the environment's (the engine's messages), or undefined.
export function channelProblem(d: EnvDescription, env: string, index?: number): string | undefined {
    const found = findPort(d, env);
    if (found === undefined)
        return "Unknown environment port: " + env;
    const addressable = found.port.addressable === true;
    if (addressable && (index === undefined || index === 0))
        return "Index required: " + env;
    if (!addressable && index !== undefined && index !== 0)
        return "No index expected: " + env;
    if (addressable && (index! < 1 || index! > (found.port.count ?? 0)))
        return `Index out of range: ${env}[${index}]`;
    return undefined;
}

export function isSensor(d: EnvDescription, env: string): boolean | undefined {
    return findPort(d, env)?.sensor;
}

// What the checks need to know of the process and of the other processes.
export interface BlockPorts {
    inputs: string[];
    outputs: string[];
}

export interface CheckContext {
    description: EnvDescription;
    blocks: Record<string, BlockPorts>;   // the process's blocks, by id
    sourced: Set<string>;                 // "block.port": inputs with a connection or a fixed value
    bindings: Binding[];                  // what stays bound meanwhile (other processes' too)
    graph: string;                        // the process
}

const idOk = (s: string) => s.length >= 1 && new TextEncoder().encode(s).length <= 64;

// The warning binding each entry would give, made in order (each against the earlier ones that
// are made), or undefined where it would be made. The engine's order of checks (04 §4).
export function checkEntries(entries: Entry[], ctx: CheckContext): (string | undefined)[] {
    const made: Binding[] = [...ctx.bindings];
    return entries.map(e => {
        const problem = checkOne(e, ctx, made);
        if (problem === undefined)
            made.push({graph: ctx.graph, ...e});
        return problem;
    });
}

function checkOne(e: Entry, ctx: CheckContext, made: Binding[]): string | undefined {
    if (!idOk(e.block) || !idOk(e.port))
        return "Invalid id";
    const channel = channelProblem(ctx.description, e.env, e.index);
    if (channel !== undefined)
        return channel;
    const block = ctx.blocks[e.block];
    if (block === undefined)
        return "Unknown node: " + e.block;
    const isInput = block.inputs.includes(e.port), isOutput = block.outputs.includes(e.port);
    if (!isInput && !isOutput)
        return `Unknown port: ${e.block}.${e.port}`;
    const sensor = isSensor(ctx.description, e.env)!;
    if (sensor !== isInput)
        return "Invalid binding direction";
    const index = e.index ?? 0;
    if (sensor) {
        const key = e.block + "." + e.port;
        if (ctx.sourced.has(key) || made.some(b => b.graph === ctx.graph && b.block === e.block && b.port === e.port))
            return `Input already connected: ${key}`;
    } else if (made.some(b => b.env === e.env && (b.index ?? 0) === index)) {
        return "Actuator already driven: " + channelLabel(e.env, index);
    }
    return undefined;
}

// ---- Bindings files (04 §4) -------------------------------------------------------------------

export interface BindingsFile {
    environment: string;
    bindings: Entry[];
}

const nonEmpty = (v: unknown): v is string => typeof v === "string" && v !== "";

// A bindings file's text: the file, or what is wrong with it (the engine's form, 04 §4).
export function parseBindingsFile(text: string): BindingsFile | string {
    let v: unknown;
    try {
        v = JSON.parse(text);
    } catch {
        return "Not a bindings file: not JSON";
    }
    if (v === null || typeof v !== "object" || Array.isArray(v))
        return "Not a bindings file: expected an object";
    const o = v as Record<string, unknown>;
    if (!nonEmpty(o.environment))
        return "environment: expected a non-empty string";
    if (!Array.isArray(o.bindings))
        return "bindings: expected an array";
    const bindings: Entry[] = [];
    for (let i = 0; i < o.bindings.length; i++) {
        const b = o.bindings[i] as Record<string, unknown>;
        if (b === null || typeof b !== "object" || Array.isArray(b))
            return `bindings[${i}]: expected an object`;
        for (const key of ["block", "port", "env"])
            if (!nonEmpty(b[key]))
                return `bindings[${i}]: ${key}: expected a non-empty string`;
        const index = b.index;
        if (index !== undefined && index !== null && (typeof index !== "number" || !Number.isInteger(index) || index < 1))
            return `bindings[${i}]: index: expected an integer of at least 1`;
        const entry: Entry = {block: b.block as string, port: b.port as string, env: b.env as string};
        if (typeof index === "number")
            entry.index = index;
        bindings.push(entry);
    }
    return {environment: o.environment, bindings};
}

// The bindings file of a process's bindings, as the engine saves one (05 §2.3).
export function bindingsFileText(environment: string, bindings: Binding[]): string {
    const entries = bindings.map(b => {
        const e: Record<string, unknown> = {block: b.block, port: b.port, env: b.env};
        if (b.index !== undefined && b.index > 0)
            e.index = b.index;
        return e;
    });
    return JSON.stringify({environment, bindings: entries}, null, 2) + "\n";
}

// ---- The mapping dialog -------------------------------------------------------------------------

// A row of the mapping dialog: a file entry and what it is mapped to ("" where nothing is
// chosen yet), or skipped.
export interface MappingRow {
    entry: Entry;
    blockPort: string;      // "block.port"
    channel: string;        // a channel label
    skip: boolean;
}

// The rows as the file gives them: what exists is kept, what does not is left to choose.
export function initialRows(entries: Entry[], ctx: CheckContext): MappingRow[] {
    return entries.map(e => {
        const block = ctx.blocks[e.block];
        const portExists = block !== undefined && (block.inputs.includes(e.port) || block.outputs.includes(e.port));
        const channelExists = channelProblem(ctx.description, e.env, e.index) === undefined;
        return {
            entry: e,
            blockPort: portExists ? e.block + "." + e.port : "",
            channel: channelExists ? channelLabel(e.env, e.index) : "",
            skip: false,
        };
    });
}

// A row as an entry; undefined while either side is not chosen.
export function rowEntry(row: MappingRow): Entry | undefined {
    if (row.blockPort === "" || row.channel === "")
        return undefined;
    const dot = row.blockPort.lastIndexOf(".");
    const {env, index} = parseChannel(row.channel);
    const e: Entry = {block: row.blockPort.substring(0, dot), port: row.blockPort.substring(dot + 1), env};
    if (index !== undefined)
        e.index = index;
    return e;
}

// Each row's problem with the current choices ("Choose a block port", a check's warning), or
// undefined where it would be made; skipped rows have none.
export function rowProblems(rows: MappingRow[], ctx: CheckContext): (string | undefined)[] {
    const active = rows.map((r, i) => ({r, i})).filter(x => !x.r.skip);
    const problems: (string | undefined)[] = rows.map(() => undefined);
    const entries: Entry[] = [];
    const where: number[] = [];
    for (const {r, i} of active) {
        const e = rowEntry(r);
        if (e === undefined) {
            problems[i] = r.blockPort === "" ? "Choose a block port" : "Choose a channel";
            continue;
        }
        entries.push(e);
        where.push(i);
    }
    checkEntries(entries, ctx).forEach((p, k) => problems[where[k]] = p);
    return problems;
}
