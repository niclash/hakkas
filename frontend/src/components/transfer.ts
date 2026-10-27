// Download, upload, backup and restore of processes (06 §4.16): the file — processes in the
// graph file form (05 §2.1) with their bindings files (04 §4), and the composite types they use as
// component graphs (02 §3.4.3) —, a graph read from its snapshot, the commands that make one
// again, and what an upload would do on a device. An upload also takes type files (03 §5.1, e.g. a
// library's), sent as their source (02 §3.5 component:source). Kept free of Vue so that it can be
// unit tested.

import {BindingsFile, parseBindingsFile} from "@/components/bindings";
import type {ProcessLink} from "@/components/protocols/link";

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
    // The links between processes (02 §3.8, plan task 79): a backup's every link; none in a
    // single process's file.
    links?: ProcessLink[];
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
    if (v.links !== undefined && !Array.isArray(v.links))
        return "links: expected an array";
    const links: ProcessLink[] = [];
    const end = (e: unknown) => isObject(e) && isString(e.graph) && isString(e.port) ? {graph: e.graph, port: e.port} : undefined;
    for (const [i, l] of (v.links ?? []).entries()) {
        const src = isObject(l) ? end(l.src) : undefined, tgt = isObject(l) ? end(l.tgt) : undefined;
        if (src === undefined || tgt === undefined)
            return `links[${i}]: expected {"src":{"graph","port"},"tgt":{"graph","port"}}`;
        links.push({src, tgt});
    }
    return {format: TRANSFER_FORMAT, version: 1, saved: isString(v.saved) ? v.saved : "",
        runtime: isObject(v.runtime) ? v.runtime : undefined, processes, types, ...(links.length > 0 ? {links} : {})};
}

// ---- Type files (03 §5.1) ------------------------------------------------------------------------

// A composite type's file, as types/ and a library hold it: sent to the engine as its source.
export interface SourceType {
    name: string;
    code: string;                   // the file's JSON, compact
    uses: string[];                 // its blocks' types
    doc: Record<string, any>;
}

// Whether v looks like a type file rather than a graph or process file: a name and blocks, an
// array (a graph file's blocks are an object).
export function isTypeSource(v: unknown): v is Record<string, any> {
    return isObject(v) && v.format === undefined && v.id === undefined && isString(v.name) && Array.isArray(v.blocks);
}

export function sourceType(v: Record<string, any>): SourceType {
    const uses = (v.blocks as unknown[]).map(b => isObject(b) && isString(b.type) ? b.type : "").filter(t => t !== "");
    return {name: v.name, code: JSON.stringify(v), uses: [...new Set(uses)], doc: v};
}

// A type file in a comparable form: what the engine's canonical form keeps (03 §5.1), the
// former "edges" as connections, initial values as text.
function sourceKey(v: Record<string, any>): string {
    const list = (x: unknown) => Array.isArray(x) ? x : [];
    const port = (p: any, ref: string) => ({name: p?.name, [ref]: p?.[ref], type: p?.type, description: p?.description,
        addressable: p?.addressable});
    return JSON.stringify({
        name: v.name, icon: v.icon ?? "", description: v.description ?? "",
        blocks: list(v.blocks).map((b: any) => [b?.name, b?.type]),
        connections: list(v.connections ?? v.edges).map((c: any) => [c?.src, c?.tgt]),
        initials: list(v.initials).map((i: any) => [String(i?.src), i?.tgt]),
        inports: list(v.inports).map((p: any) => port(p, "tgt")),
        outports: list(v.outports).map((p: any) => port(p, "src")),
    });
}

// Whether the type file and the device's source of that type (component:getsource) are the same.
export function sameSource(a: SourceType, deviceCode: string): boolean {
    try {
        const b = JSON.parse(deviceCode);
        return isObject(b) && sourceKey(a.doc) === sourceKey(b);
    } catch {
        return false;
    }
}

// What an upload of one or more files holds: their processes and component graphs (several
// process files together, one file's process replacing an earlier one's of that id), and their
// type files.
export interface UploadSet {
    file: TransferFile;
    sources: SourceType[];
}

export function parseUpload(files: { name: string, text: string }[]): UploadSet | string {
    const processes = new Map<string, ProcessEntry>();
    const types = new Map<string, TypeFile>();
    const sources = new Map<string, SourceType>();
    const links: ProcessLink[] = [];
    let runtime: TransferFile["runtime"], saved = "";
    for (const f of files) {
        let v: unknown;
        try {
            v = JSON.parse(f.text);
        } catch {
            return f.name + ": not JSON";
        }
        if (isTypeSource(v)) {
            sources.set(v.name, sourceType(v));
            continue;
        }
        const t = parseTransferFile(f.text);
        if (typeof t === "string")
            return f.name + ": " + t;
        t.processes.forEach(p => processes.set(p.graph.id, p));
        t.types.forEach(x => types.has(x.id) || types.set(x.id, x));
        links.push(...(t.links ?? []));
        runtime = runtime ?? t.runtime;
        saved = saved || t.saved;
    }
    return {file: {format: TRANSFER_FORMAT, version: 1, saved, runtime, processes: [...processes.values()],
        types: [...types.values()], ...(links.length > 0 ? {links} : {})}, sources: [...sources.values()]};
}

// The type files in an order where each comes after those (of the list) it uses.
export function sourceOrder(sources: SourceType[]): SourceType[] {
    const byName = new Map(sources.map(t => [t.name, t]));
    const done = new Set<string>(), visiting = new Set<string>();
    const out: SourceType[] = [];
    const visit = (t: SourceType) => {
        if (done.has(t.name) || visiting.has(t.name))
            return;
        visiting.add(t.name);
        t.uses.forEach(u => byName.has(u) && visit(byName.get(u)!));
        visiting.delete(t.name);
        done.add(t.name);
        out.push(t);
    };
    sources.forEach(visit);
    return out;
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
    // The sources (component:getsource) of the device's composite types an upload's type files
    // name.
    sources?: Map<string, string>;
    environment?: string;   // the environment's name; undefined without one
    links?: ProcessLink[];  // the links between the device's processes
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

export interface SourcePlan {
    source: SourceType;
    action: TypeAction;
}

export interface UploadPlan {
    sources: SourcePlan[];      // the type files, made first, in this order
    types: TypePlan[];          // in the order they are made
    processes: ProcessPlan[];
    fileLinks: ProcessLink[];   // the file's links (a backup's)
    deviceLinks: ProcessLink[]; // the device's, of which a replaced process's are made again
}

export function uploadPlan(file: TransferFile, device: DeviceState, sourceFiles: SourceType[] = []): UploadPlan {
    const sources = sourceOrder(sourceFiles).map((source): SourcePlan => {
        const there = device.types.get(source.name);
        const code = device.sources?.get(source.name);
        const action: TypeAction = there === undefined ? "add" : there === null ? "builtin"
            : code !== undefined && sameSource(source, code) ? "same" : "different";
        return {source, action};
    });
    const types = typeOrder(file.types).map((type): TypePlan => {
        const there = device.types.get(type.id);
        const action: TypeAction = there === undefined ? "add" : there === null ? "builtin"
            : sameInside(there, type) ? "same" : "different";
        return {type, action};
    });
    const inFile = new Set([...file.types.map(t => t.id), ...sourceFiles.map(t => t.name)]);
    const processes = file.processes.map((entry): ProcessPlan => {
        const there = device.processes.get(entry.graph.id);
        const missing = [...componentsOf(entry.graph)].filter(c => !device.types.has(c) && !inFile.has(c)).sort();
        const bindings = entry.bindings === undefined || entry.bindings.bindings.length === 0 ? "none"
            : device.environment === undefined ? "no-environment" : "apply";
        return {entry, include: true, missing, bindings, ...(there !== undefined ? {replaces: there} : {})};
    });
    return {sources, types, processes, fileLinks: file.links ?? [], deviceLinks: device.links ?? []};
}

// The links an upload makes once its processes are made (06 §4.16, plan task 79): the file's,
// and the device's to and from each process it replaces — deleting a process ends its links, and
// deploying a process again keeps them. Each once, the file's first.
export function linksToMake(plan: UploadPlan): ProcessLink[] {
    const replaced = new Set(plan.processes.filter(p => p.include && p.replaces).map(p => p.entry.graph.id));
    const out: ProcessLink[] = [];
    const key = (l: ProcessLink) => JSON.stringify([l.src.graph, l.src.port, l.tgt.graph, l.tgt.port]);
    const seen = new Set<string>();
    for (const l of [...plan.fileLinks, ...plan.deviceLinks.filter(l => replaced.has(l.src.graph) || replaced.has(l.tgt.graph))]) {
        if (!seen.has(key(l))) {
            seen.add(key(l));
            out.push(l);
        }
    }
    return out;
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
