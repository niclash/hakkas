// Download, backup and upload (06 §4.16), over the transfer's own connection: reading the
// processes and types into a file, and making a file's types and processes on the engine.

import {vueModel} from "@/components/model";
import {environment} from "@/components/environment";
import type {Binding, EnvDescription} from "@/components/protocols/environment";
import {CheckContext, checkEntries, Entry, initialRows, MappingRow} from "@/components/bindings";
import {TransferConnection, TransferError} from "@/components/transfer-connection";
import {
    componentsOf, DeviceState, GraphFile, insideCommands, portsOf, ProcessEntry, ProcessPlan, SourcePlan, SourceType,
    TransferFile, transferFile, TypeFile, TypePlan, uploadPlan, UploadPlan
} from "@/components/transfer";

const composites = () => new Set(vueModel.components.value.filter(c => c.subgraph).map(c => c.name));

function bindingsEntry(environmentName: string | undefined, bindings: Binding[] | undefined, graph: string): ProcessEntry["bindings"] {
    const mine = (bindings ?? []).filter(b => b.graph === graph);
    if (environmentName === undefined || mine.length === 0)
        return undefined;
    return {environment: environmentName, bindings: mine.map(b => {
        const e: Entry = {port: b.port, env: b.env};
        if (b.index !== undefined && b.index > 0)
            e.index = b.index;
        return e;
    })};
}

// The processes (all of them: a backup) with their bindings, and the composite types: for a
// backup every one, else those the processes use (and the types those use).
export async function readFile(ids: string[] | "all"): Promise<TransferFile> {
    const t = await TransferConnection.open();
    try {
        const all = ids === "all";
        const wanted = all ? [...vueModel.graphs.value.keys()] : ids;
        const bindings = await t.bindings();
        const envName = bindings === undefined ? undefined : environment.description?.name;
        const processes: ProcessEntry[] = [];
        for (const id of wanted) {
            const graph = await t.read(id);
            graph.started = vueModel.graphs.value.get(id)?.started ?? false;
            const b = bindingsEntry(envName, bindings, id);
            processes.push(b !== undefined ? {graph, bindings: b} : {graph});
        }
        const known = composites();
        const types: TypeFile[] = [];
        const read = new Set<string>();
        let next = all ? [...known] : processes.flatMap(p => [...componentsOf(p.graph)]).filter(c => known.has(c));
        while (next.length > 0) {
            const name = next.shift()!;
            if (read.has(name))
                continue;
            read.add(name);
            const type = await t.read(name) as TypeFile;
            types.push(type);
            next.push(...[...componentsOf(type)].filter(c => known.has(c) && !read.has(c)));
        }
        const r = vueModel.runtime.value ?? {};
        const runtime = {label: String(r.label ?? ""), id: String(r.id ?? ""), version: String(r.version ?? "")};
        return transferFile(processes, types, runtime);
    } finally {
        t.close();
    }
}

// What an upload of the files would do here, read over a transfer connection kept for the upload.
export async function planUpload(t: TransferConnection, file: TransferFile, sourceFiles: SourceType[] = []): Promise<UploadPlan> {
    const types = new Map<string, TypeFile | null>();
    const sources = new Map<string, string>();
    const known = composites();
    const inFile = new Set(file.types.map(x => x.id));
    for (const s of sourceFiles)
        if (known.has(s.name))
            sources.set(s.name, await t.source(s.name));
    // A composite type the file has too is read, to compare; a built-in one is null.
    for (const c of vueModel.components.value) {
        if (!known.has(c.name))
            types.set(c.name, null);
        else
            types.set(c.name, inFile.has(c.name) ? await t.read(c.name) as TypeFile : emptyType(c.name));
    }
    const device: DeviceState = {
        processes: new Map([...vueModel.graphs.value.entries()].map(([id, g]) => [id, {name: g.name, running: g.running}])),
        types,
        sources,
        environment: environment.description?.name,
    };
    return uploadPlan(file, device, sourceFiles);
}

// A composite type of the device not compared (the file has none of that name).
function emptyType(id: string): TypeFile {
    return {id, name: id, description: "", icon: "", started: false, blocks: {}, connections: [], inports: [], outports: [], library: ""};
}

export interface ReportLine {
    kind: "type" | "process";
    id: string;
    name: string;
    ok: boolean;
    notes: string[];
}

export interface UploadHooks {
    progress: (text: string) => void;
    // The mapping dialog for a process's bindings: the entries to bind, or undefined (cancelled).
    mapping: (process: string, rows: MappingRow[], context: CheckContext, mismatch?: string) => Promise<Entry[] | undefined>;
}

const keptNote = (action: TypePlan["action"]) => action === "builtin" ? "a built-in type of this name is on the device: kept"
    : "on the device and different: the device's is kept";

// A type file, registered as the engine registers one at startup (02 §3.5 component:source).
async function makeSource(t: TransferConnection, p: SourcePlan): Promise<ReportLine> {
    const line: ReportLine = {kind: "type", id: p.source.name, name: p.source.name, ok: true, notes: []};
    if (p.action === "same")
        return {...line, notes: ["already on the device"]};
    if (p.action !== "add")
        return {...line, ok: false, notes: [keptNote(p.action)]};
    const refused = await t.addSource(p.source.name, p.source.code);
    return refused === "" ? line : {...line, ok: false, notes: [refused]};
}

async function makeType(t: TransferConnection, p: TypePlan): Promise<ReportLine> {
    const type = p.type;
    const line: ReportLine = {kind: "type", id: type.id, name: type.id, ok: true, notes: []};
    if (p.action === "same")
        return {...line, notes: ["already on the device"]};
    if (p.action !== "add")
        return {...line, ok: false, notes: [keptNote(p.action)]};
    t.refusals(type.id);
    const made = t.seen((protocol, command, payload) => protocol === "component" && command === "component" && payload?.name === type.id);
    const label = type.id.substring(type.id.lastIndexOf("/") + 1);
    t.send("graph", "clear", {id: type.id, name: type.name || label, library: type.library, main: false,
        description: type.description, icon: type.icon});
    for (const c of insideCommands(type, type.id))
        t.send(c.protocol, c.command, c.payload);
    t.send("network", "persist", {graph: type.id});
    await t.sync();
    made.stop();
    line.notes = t.refusals(type.id);
    if (!made.value) {
        line.ok = false;
        // Its half-built component graph goes again.
        t.send("runtime", "delete_graph", {id: type.id, name: type.name || label});
        await t.sync();
        t.refusals(type.id);
    }
    return line;
}

async function makeProcess(t: TransferConnection, p: ProcessPlan, description: EnvDescription | undefined,
                           hooks: UploadHooks): Promise<ReportLine> {
    const g: GraphFile = p.entry.graph;
    const id = g.id;
    const line: ReportLine = {kind: "process", id, name: g.name, ok: true, notes: []};
    const answer = (graph: string, command: string) => t.waitFor((protocol, c, payload) => {
        if (protocol === "runtime" && c === command && payload?.graph === graph)
            return "";
        if (protocol === "network" && c === "error" && payload?.graph === graph)
            return String(payload.message);
        return undefined;
    });
    t.refusals(id);
    if (p.replaces !== undefined) {
        hooks.progress(`Replacing “${p.replaces.name}”…`);
        const deleted = answer(id, "delete_graph");
        t.send("runtime", "delete_graph", {id, name: p.replaces.name});
        const refused = await deleted;
        if (refused !== "")
            return {...line, ok: false, notes: ["not replaced: " + refused]};
    }
    hooks.progress(`Uploading “${g.name}”…`);
    const created = answer(id, "new_graph");
    t.send("runtime", "new_graph", {id, name: g.name, description: g.description, icon: g.icon});
    const refused = await created;
    if (refused !== "")
        return {...line, ok: false, notes: ["not created: " + refused]};
    for (const c of insideCommands(g, id))
        t.send(c.protocol, c.command, c.payload);
    await t.sync();
    line.notes.push(...t.refusals(id));

    // Its bindings, checked against what the engine made of it (06 §4.16).
    const file = p.entry.bindings;
    if (file !== undefined && file.bindings.length > 0) {
        if (description === undefined)
            line.notes.push("bindings not applied: this engine has no environment");
        else {
            const made = await t.read(id);
            const bound = (await t.bindings()) ?? [];
            const context: CheckContext = {description, ...portsOf(made), bindings: bound.filter(b => b.graph !== id), graph: id};
            const mismatch = file.environment !== description.name
                ? `Written for the environment “${file.environment}”; this one is “${description.name}”.` : undefined;
            let entries: Entry[] | undefined = file.bindings;
            if (mismatch !== undefined || checkEntries(file.bindings, context).some(w => w !== undefined))
                entries = await hooks.mapping(g.name, initialRows(file.bindings, context), context, mismatch);
            if (entries === undefined)
                line.notes.push("bindings not applied");
            else {
                for (const e of entries)
                    t.send("environment", "bind", {graph: id, ...e});
                await t.sync();
                const notes = t.refusals(id);
                line.notes.push(...notes, `${entries.length - notes.length} binding${entries.length - notes.length === 1 ? "" : "s"} applied`);
            }
        }
    }
    t.send("network", "persist", {graph: id});
    await t.sync();
    const saveNotes = t.refusals(id);
    line.notes.push(...saveNotes);
    if (saveNotes.length > 0)
        line.ok = false;
    return line;
}

// Makes the plan's type files, its types and its included processes; each process stays
// stopped. A lost connection ends it, what was made staying.
export async function runUpload(t: TransferConnection, plan: UploadPlan, hooks: UploadHooks): Promise<ReportLine[]> {
    const report: ReportLine[] = [];
    try {
        for (const p of plan.sources) {
            hooks.progress(`Type ${p.source.name}…`);
            report.push(await makeSource(t, p));
        }
        for (const p of plan.types) {
            hooks.progress(`Type ${p.type.id}…`);
            report.push(await makeType(t, p));
        }
        for (const p of plan.processes.filter(x => x.include))
            report.push(await makeProcess(t, p, environment.description, hooks));
    } catch (e) {
        report.push({kind: "process", id: "", name: "", ok: false,
            notes: [(e instanceof TransferError ? e.message : String(e)) + ": the upload stopped here"]});
    }
    return report;
}
