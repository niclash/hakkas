// Download, upload, backup and restore of processes (06 §4.16): the file — processes in the
// graph file form (05 §2.1) with their bindings files (04 §4), and the composite types they use as
// component graphs (02 §3.4.3) —, a graph read from its snapshot, the commands that make one
// again, and what an upload would do on a device. Kept free of Vue so that it can be unit tested.

import {BindingsFile, parseBindingsFile} from "@/components/bindings";

export const TRANSFER_FORMAT = "hakkas/processes";

export interface BlockRef {
    block: string;
    port: string;
}

// A graph in the graph file's form (05 §2.1). Initials keep the engine's form, a string (or,
// from a file the engine wrote, a number or boolean).
export interface GraphFile {
    id: string;
    name: string;
    description: string;
    icon: string;
    started: boolean;
    blocks: Record<string, { component: string, metadata: Record<string, unknown> }>;
    connections: ({ src: BlockRef, tgt: BlockRef, metadata?: Record<string, unknown> }
        | { src: { data: string | number | boolean }, tgt: BlockRef })[];
    inports: { name: string, block: string, port: string }[];
    outports: { name: string, block: string, port: string }[];
}

// A composite type: its component graph, `id` its name.
export interface TypeFile extends GraphFile {
    library: string;
}

export interface ProcessEntry {
    graph: GraphFile;
    bindings?: BindingsFile;
}

export interface TransferFile {
    format: typeof TRANSFER_FORMAT;
    version: 1;
    saved: string;
    runtime?: { label?: string, id?: string, version?: string };
    processes: ProcessEntry[];
    types: TypeFile[];
}

export function emptyGraph(id: string): GraphFile {
    return {id, name: id, description: "", icon: "", started: false, blocks: {}, connections: [], inports: [], outports: []};
}

// ---- A graph from its snapshot (02 §4.1.1) -------------------------------------------------------

export interface SnapshotMessage {
    command: string;
    payload: any;
}

// The graph a graph:connect snapshot shows: its graph:clear, then its blocks, process ports,
// initials and connections. Other messages (later events, other graphs') are left out.
export function graphFromSnapshot(messages: SnapshotMessage[]): GraphFile | TypeFile | undefined {
    const clear = messages.find(m => m.command === "clear")?.payload;
    if (clear === undefined)
        return undefined;
    const g: GraphFile = {...emptyGraph(String(clear.id)), name: String(clear.name ?? clear.id),
        description: String(clear.description ?? ""), icon: String(clear.icon ?? "")};
    for (const {command, payload: p} of messages) {
        if (p?.graph !== g.id)
            continue;
        switch (command) {
            case "addnode":
                g.blocks[p.id] = {component: p.component, metadata: {...(p.metadata ?? {})}};
                break;
            case "addinport":
            case "addoutport":
                (command === "addinport" ? g.inports : g.outports).push({name: p.name ?? p.public, block: p.node, port: p.port});
                break;
            case "addinitial":
                g.connections.push({src: {data: p.src.data}, tgt: {block: p.tgt.node, port: p.tgt.port}});
                break;
            case "addedge": {
                const c: GraphFile["connections"][number] = {src: {block: p.src.node, port: p.src.port},
                    tgt: {block: p.tgt.node, port: p.tgt.port}};
                if (p.metadata !== undefined && p.metadata !== null && Object.keys(p.metadata).length > 0)
                    c.metadata = {...p.metadata};
                g.connections.push(c);
                break;
            }
        }
    }
    return clear.main === false ? {...g, library: String(clear.library ?? "")} : g;
}

// ---- The file ------------------------------------------------------------------------------------

export function transferFile(processes: ProcessEntry[], types: TypeFile[], runtime: TransferFile["runtime"],
                             now: Date = new Date()): TransferFile {
    return {format: TRANSFER_FORMAT, version: 1, saved: now.toISOString(), runtime, processes, types};
}

export function transferText(file: TransferFile): string {
    return JSON.stringify(file, null, 2) + "\n";
}

// A name for a downloaded file: what is safe in a file name on any system.
export function fileName(name: string, suffix: string): string {
    const safe = name.replace(/[^A-Za-z0-9_. -]+/g, "_").trim() || "hakkas";
    return safe + suffix;
}

const isObject = (v: unknown): v is Record<string, any> => v !== null && typeof v === "object" && !Array.isArray(v);
const isString = (v: unknown): v is string => typeof v === "string";

// A reference to a block's port; the loader's other spellings ("node", "process") too (05 §2.1).
function ref(v: unknown): BlockRef | undefined {
    if (!isObject(v) || !isString(v.port))
        return undefined;
    const names = ["block", "node", "process"].filter(k => v[k] !== undefined);
    return names.length === 1 && isString(v[names[0]]) ? {block: v[names[0]], port: v.port} : undefined;
}

// A graph file, or what is wrong with it (`at` names it in the message).
function parseGraph(v: unknown, at: string): GraphFile | string {
    if (!isObject(v))
        return `${at}: expected an object`;
    if (!isString(v.id) || v.id === "" || new TextEncoder().encode(v.id).length > 64)
        return `${at}: id: expected a graph id`;
    for (const key of ["name", "description", "icon"])
        if (v[key] !== undefined && !isString(v[key]))
            return `${at}: ${key}: expected a string`;
    if (v.started !== undefined && typeof v.started !== "boolean")
        return `${at}: started: expected true or false`;
    if (v.blocks !== undefined && !isObject(v.blocks))
        return `${at}: blocks: expected an object`;
    for (const key of ["connections", "inports", "outports"])
        if (v[key] !== undefined && !Array.isArray(v[key]))
            return `${at}: ${key}: expected an array`;
    const g = emptyGraph(v.id);
    g.name = v.name ?? v.id;
    g.description = v.description ?? "";
    g.icon = v.icon ?? "";
    g.started = v.started ?? false;
    for (const [id, b] of Object.entries(v.blocks ?? {})) {
        if (!isObject(b) || !isString(b.component) || (b.metadata !== undefined && !isObject(b.metadata)))
            return `${at}: blocks.${id}: expected {component, metadata}`;
        g.blocks[id] = {component: b.component, metadata: {...(b.metadata ?? {})}};
    }
    for (const [i, c] of (v.connections ?? []).entries()) {
        const tgt = isObject(c) ? ref(c.tgt) : undefined;
        if (!isObject(c) || tgt === undefined || !isObject(c.src))
            return `${at}: connections[${i}]: expected {src, tgt}`;
        if (c.src.data !== undefined) {
            if (!["string", "number", "boolean"].includes(typeof c.src.data))
                return `${at}: connections[${i}]: src.data: expected a string, number or boolean`;
            g.connections.push({src: {data: c.src.data}, tgt});
            continue;
        }
        const src = ref(c.src);
        if (src === undefined || (c.metadata !== undefined && !isObject(c.metadata)))
            return `${at}: connections[${i}]: expected {src, tgt}`;
        g.connections.push(c.metadata !== undefined ? {src, tgt, metadata: {...c.metadata}} : {src, tgt});
    }
    for (const key of ["inports", "outports"] as const) {
        for (const [i, p] of (v[key] ?? []).entries()) {
            const r = isObject(p) ? ref(p) : undefined;
            if (!isObject(p) || !isString(p.name) || r === undefined)
                return `${at}: ${key}[${i}]: expected {name, block, port}`;
            g[key].push({name: p.name, ...r});
        }
    }
    return g;
}

// A transfer file (or a plain graph file, as one process), or what is wrong with it.
export function parseTransferFile(text: string): TransferFile | string {
    let v: unknown;
    try {
        v = JSON.parse(text);
    } catch {
        return "Not a process file: not JSON";
    }
    if (!isObject(v))
        return "Not a process file: expected an object";
    if (v.format === undefined && v.id !== undefined) {
        const g = parseGraph(v, "graph");
        return typeof g === "string" ? g : transferFile([{graph: g}], [], undefined, new Date(0));
    }
    if (v.format !== TRANSFER_FORMAT)
        return "Not a process file: format: expected " + TRANSFER_FORMAT;
    if (v.version !== 1)
        return "version: expected 1";
    if (!Array.isArray(v.processes))
        return "processes: expected an array";
    if (v.types !== undefined && !Array.isArray(v.types))
        return "types: expected an array";
    const processes: ProcessEntry[] = [];
    for (const [i, p] of v.processes.entries()) {
        if (!isObject(p))
            return `processes[${i}]: expected an object`;
        const graph = parseGraph(p.graph, `processes[${i}].graph`);
        if (typeof graph === "string")
            return graph;
        const entry: ProcessEntry = {graph};
        if (p.bindings !== undefined) {
            const b = parseBindingsFile(JSON.stringify(p.bindings));
            if (typeof b === "string")
                return `processes[${i}].bindings: ${b}`;
            entry.bindings = b;
        }
        processes.push(entry);
    }
    const types: TypeFile[] = [];
    for (const [i, t] of (v.types ?? []).entries()) {
        const graph = parseGraph(t, `types[${i}]`);
        if (typeof graph === "string")
            return graph;
        types.push({...graph, library: graph.id.includes("/") ? graph.id.split("/")[0] : ""});
    }
    return {format: TRANSFER_FORMAT, version: 1, saved: isString(v.saved) ? v.saved : "",
        runtime: isObject(v.runtime) ? v.runtime : undefined, processes, types};
}

// ---- Types ---------------------------------------------------------------------------------------

export function componentsOf(g: GraphFile): Set<string> {
    return new Set(Object.values(g.blocks).map(b => b.component));
}

// The composite types the graphs use, and the types those use, among `types` (by name).
export function typesUsed(graphs: GraphFile[], types: Map<string, TypeFile>): string[] {
    const seen = new Set<string>();
    const visit = (g: GraphFile) => {
        for (const c of componentsOf(g)) {
            const t = types.get(c);
            if (t !== undefined && !seen.has(c)) {
                seen.add(c);
                visit(t);
            }
        }
    };
    graphs.forEach(visit);
    return [...seen];
}

// The types in an order where each comes after the types (of the list) it uses; a cycle keeps
// the file's order (the engine refuses a recursive type).
export function typeOrder(types: TypeFile[]): TypeFile[] {
    const byName = new Map(types.map(t => [t.id, t]));
    const done = new Set<string>(), visiting = new Set<string>();
    const out: TypeFile[] = [];
    const visit = (t: TypeFile) => {
        if (done.has(t.id) || visiting.has(t.id))
            return;
        visiting.add(t.id);
        for (const c of componentsOf(t)) {
            const dep = byName.get(c);
            if (dep !== undefined)
                visit(dep);
        }
        visiting.delete(t.id);
        done.add(t.id);
        out.push(t);
    };
    types.forEach(visit);
    return out;
}

// Whether two graphs have the same blocks, connections, initials and ports; positions and other
// metadata do not count. An initial's value is compared as text ("5" and 5 are the same).
export function sameInside(a: GraphFile, b: GraphFile): boolean {
    const key = (g: GraphFile) => JSON.stringify({
        blocks: Object.entries(g.blocks).map(([id, x]) => id + "\u0000" + x.component).sort(),
        connections: g.connections.map(c => "data" in c.src
            ? `=${String(c.src.data)}>${c.tgt.block}.${c.tgt.port}`
            : `${c.src.block}.${c.src.port}>${c.tgt.block}.${c.tgt.port}`).sort(),
        inports: g.inports.map(p => `${p.name}=${p.block}.${p.port}`).sort(),
        outports: g.outports.map(p => `${p.name}=${p.block}.${p.port}`).sort(),
    });
    return key(a) === key(b);
}

// ---- The commands ----------------------------------------------------------------------------------

export interface Command {
    protocol: "graph";
    command: string;
    payload: Record<string, unknown>;
}

// The graph commands that make g's inside in the graph `graph`, in the snapshot's order: the
// blocks with their metadata, the process ports, the initials, the connections.
export function insideCommands(g: GraphFile, graph: string): Command[] {
    const cmds: Command[] = [];
    const add = (command: string, payload: Record<string, unknown>) => cmds.push({protocol: "graph", command, payload: {graph, ...payload}});
    for (const [id, b] of Object.entries(g.blocks))
        add("addnode", {id, component: b.component, metadata: {...b.metadata}});
    for (const p of g.inports)
        add("addinport", {public: p.name, node: p.block, port: p.port});
    for (const p of g.outports)
        add("addoutport", {public: p.name, node: p.block, port: p.port});
    for (const c of g.connections)
        if ("data" in c.src)
            add("addinitial", {src: {data: c.src.data}, tgt: {node: c.tgt.block, port: c.tgt.port}});
    for (const c of g.connections) {
        if (!("data" in c.src)) {
            const src = c.src as BlockRef;
            add("addedge", {src: {node: src.block, port: src.port}, tgt: {node: c.tgt.block, port: c.tgt.port},
                ...("metadata" in c && c.metadata !== undefined ? {metadata: {...c.metadata}} : {})});
        }
    }
    return cmds;
}

// ---- What an upload would do -----------------------------------------------------------------------

export interface DeviceState {
    processes: Map<string, { name: string, running: boolean }>;
    // The device's block types: a composite one with its component graph (read when needed),
    // a built-in one as null.
    types: Map<string, TypeFile | null>;
    environment?: string;   // the environment's name; undefined without one
}

export type TypeAction = "add" | "same" | "different" | "builtin";

export interface TypePlan {
    type: TypeFile;
    action: TypeAction;
}

export interface ProcessPlan {
    entry: ProcessEntry;
    replaces?: { name: string, running: boolean };
    include: boolean;
    // The block types it uses that are neither on the device nor in the file.
    missing: string[];
    bindings: "none" | "apply" | "no-environment";
}

export interface UploadPlan {
    types: TypePlan[];          // in the order they are made
    processes: ProcessPlan[];
}

export function uploadPlan(file: TransferFile, device: DeviceState): UploadPlan {
    const types = typeOrder(file.types).map((type): TypePlan => {
        const there = device.types.get(type.id);
        const action: TypeAction = there === undefined ? "add" : there === null ? "builtin"
            : sameInside(there, type) ? "same" : "different";
        return {type, action};
    });
    const inFile = new Set(file.types.map(t => t.id));
    const processes = file.processes.map((entry): ProcessPlan => {
        const there = device.processes.get(entry.graph.id);
        const missing = [...componentsOf(entry.graph)].filter(c => !device.types.has(c) && !inFile.has(c)).sort();
        const bindings = entry.bindings === undefined || entry.bindings.bindings.length === 0 ? "none"
            : device.environment === undefined ? "no-environment" : "apply";
        return {entry, include: true, missing, bindings, ...(there !== undefined ? {replaces: there} : {})};
    });
    return {types, processes};
}

// What the bindings checks (bindings.ts) need of a process, from its graph as read back.
export function portsOf(g: GraphFile): { inports: Record<string, string>, outports: Record<string, string>, sourced: Set<string> } {
    const inports: Record<string, string> = {}, outports: Record<string, string> = {};
    for (const p of g.inports)
        inports[p.name] = p.block + "." + p.port;
    for (const p of g.outports)
        outports[p.name] = p.block + "." + p.port;
    return {inports, outports, sourced: new Set(g.connections.map(c => c.tgt.block + "." + c.tgt.port))};
}
