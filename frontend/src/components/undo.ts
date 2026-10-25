// Undo and redo (06 §4.13): this tab's own edits of a process, as steps of graph commands
// with their inverses. Undoing sends the inverse commands — the engine stays the source of
// truth — after checking that the process still is as the step left it; a step that someone
// else has changed meanwhile is skipped. Kept free of Vue and Rete so that it can be unit tested.

export interface Cmd {
    command: string;
    payload: Record<string, any>;
}

export interface Step {
    graph: string;
    forward: Cmd[];   // what was done (redo sends it again)
    inverse: Cmd[];   // what undoes it
}

export interface End {
    node: string;
    port: string;
}

export interface Link {
    src: End;
    tgt: End;
}

// The process as the editor shows it, read when a command is sent (before its event) and when
// a step is undone or redone.
export interface StateReader {
    block(id: string): { component: string, x: number, y: number } | undefined;
    initial(node: string, port: string): string | undefined;
    initials(node: string): Record<string, string>;   // the block's fixed values, by input
    edges(node: string): Link[];                   // into and out of the block
    edgeInto(node: string, port: string): Link | undefined;
    ports(node: string): { direction: "in" | "out", name: string, port: string }[];
    port(direction: "in" | "out", name: string): End | undefined;
    // A process port's environment bindings (06 §4.14; plan task 70), and whether the port of
    // that name is bound to a channel ("ain[2]"; the channel says the direction, 04 §4).
    portBindings(direction: "in" | "out", name: string): { env: string, index?: number }[];
    bound(port: string, channel: string): boolean;
}

// The commands that can be undone; any other is not recorded. All are graph:*, but bind and
// unbind, which are environment:* (02 §3.7).
export const UNDOABLE = new Set(["addnode", "removenode", "renamenode", "changenode", "addedge", "removeedge",
    "addinitial", "removeinitial", "addinport", "removeinport", "renameinport", "addoutport", "removeoutport",
    "renameoutport", "bind", "unbind"]);

export const ENVIRONMENT_COMMANDS = new Set(["bind", "unbind"]);

const channel = (env: string, index?: number) => index !== undefined && index > 0 ? `${env}[${index}]` : env;
const bindCmd = (command: string, graph: string, port: string, env: string, index?: number): Cmd =>
    ({command, payload: index !== undefined && index > 0 ? {graph, port, env, index} : {graph, port, env}});

// A process port and its bindings, made again (its bindings went with it, 04 §4).
const portBack = (graph: string, direction: "in" | "out", node: string, port: string, name: string, s: StateReader): Cmd[] => [
    {command: direction === "in" ? "addinport" : "addoutport", payload: {graph, node, port, public: name}},
    ...s.portBindings(direction, name).map(b => bindCmd("bind", graph, name, b.env, b.index)),
];

export const HISTORY_SIZE = 500;

const dirOf = (command: string): "in" | "out" => command.includes("inport") ? "in" : "out";
const edgeCmd = (command: string, graph: string, l: Link): Cmd => ({command, payload: {graph, src: {...l.src}, tgt: {...l.tgt}}});

// The commands that undo cmd, in the state before cmd.
export function inverseOf(cmd: Cmd, s: StateReader): Cmd[] {
    const p = cmd.payload, graph = p.graph;
    switch (cmd.command) {
        case "addnode":
            return [{command: "removenode", payload: {graph, id: p.id}}];
        case "removenode": {
            const b = s.block(p.id);
            if (b === undefined)
                return [];
            const out: Cmd[] = [{command: "addnode", payload: {graph, id: p.id, component: b.component, metadata: {x: b.x, y: b.y}}}];
            for (const l of s.edges(p.id))
                out.push(edgeCmd("addedge", graph, l));
            for (const [port, data] of Object.entries(s.initials(p.id)))
                out.push({command: "addinitial", payload: {graph, tgt: {node: p.id, port}, src: {data}}});
            for (const gp of s.ports(p.id))
                out.push(...portBack(graph, gp.direction, p.id, gp.port, gp.name, s));
            return out;
        }
        case "renamenode":
            return [{command: "renamenode", payload: {graph, from: p.to, to: p.from}}];
        case "changenode": {
            const b = s.block(p.id);
            return b === undefined ? [] : [{command: "changenode", payload: {graph, id: p.id, metadata: {x: b.x, y: b.y}}}];
        }
        case "addedge":
            return [edgeCmd("removeedge", graph, p as Link)];
        case "removeedge":
            return [edgeCmd("addedge", graph, p as Link)];
        case "addinitial": {
            const old = s.initial(p.tgt.node, p.tgt.port);
            return old === undefined
                ? [{command: "removeinitial", payload: {graph, tgt: {...p.tgt}}}]
                : [{command: "addinitial", payload: {graph, tgt: {...p.tgt}, src: {data: old}}}];
        }
        case "removeinitial": {
            const old = s.initial(p.tgt.node, p.tgt.port);
            return old === undefined ? [] : [{command: "addinitial", payload: {graph, tgt: {...p.tgt}, src: {data: old}}}];
        }
        case "addinport":
        case "addoutport":
            return [{command: cmd.command.replace("add", "remove"), payload: {graph, public: p.public}}];
        case "removeinport":
        case "removeoutport": {
            const at = s.port(dirOf(cmd.command), p.public);
            return at === undefined ? [] : portBack(graph, dirOf(cmd.command), at.node, at.port, p.public, s);
        }
        case "renameinport":
        case "renameoutport":
            return [{command: cmd.command, payload: {graph, from: p.to, to: p.from}}];
        case "bind":
            return [bindCmd("unbind", graph, p.port, p.env, p.index)];
        case "unbind":
            return s.bound(p.port, channel(p.env, p.index)) ? [bindCmd("bind", graph, p.port, p.env, p.index)] : [];
    }
    return [];
}

// Removals before additions, each in the order that keeps every command valid: ports go before
// initials, edges and blocks; blocks come back before their edges, initials and ports. Renames
// and moves keep their place among themselves, first.
const phase: Record<string, number> = {
    renamenode: 0, changenode: 0, renameinport: 0, renameoutport: 0,
    removeinport: 1, removeoutport: 1, unbind: 1, removeinitial: 2, removeedge: 3, removenode: 4,
    addnode: 5, addedge: 6, addinitial: 7, addinport: 8, addoutport: 8, bind: 9,
};

export function orderCommands(cmds: Cmd[]): Cmd[] {
    const seen = new Set<string>();
    const unique = cmds.filter(c => {
        const key = JSON.stringify(c);
        if (seen.has(key))
            return false;
        seen.add(key);
        return true;
    });
    return unique.map((c, i) => ({c, i})).sort((a, b) => (phase[a.c.command] ?? 0) - (phase[b.c.command] ?? 0) || a.i - b.i)
        .map(x => x.c);
}

// A step from the commands of one user action, with the state before them: the inverse is the
// commands' inverses in reverse, ordered; a block moved several times (a drag) goes back to
// where it was before the first move, and forward to its last place.
export function makeStep(graph: string, cmds: Cmd[], inverses: Cmd[][]): Step {
    const moved = new Set<string>();
    const inv: Cmd[] = [];
    for (let i = 0; i < cmds.length; i++) {
        if (cmds[i].command === "changenode") {
            if (moved.has(cmds[i].payload.id))
                continue;
            moved.add(cmds[i].payload.id);
        }
        inv.unshift(...inverses[i]);
    }
    const lastMove = new Map<string, number>();
    cmds.forEach((c, i) => c.command === "changenode" && lastMove.set(c.payload.id, i));
    const forward = cmds.filter((c, i) => c.command !== "changenode" || lastMove.get(c.payload.id) === i);
    return {graph, forward, inverse: orderCommands(inv)};
}

// What a list of commands leaves behind, as facts to check: the last word on each thing wins.
export function factsOf(cmds: Cmd[]): Map<string, string | null> {
    const f = new Map<string, string | null>();
    const end = (e: End) => e.node + "." + e.port;
    for (const c of cmds) {
        const p = c.payload;
        switch (c.command) {
            case "addnode":
                f.set("block:" + p.id, p.component);
                break;
            case "removenode":
                f.set("block:" + p.id, null);
                break;
            case "renamenode":
                f.set("block:" + p.from, null);
                f.set("exists:" + p.to, "yes");
                break;
            case "changenode":
                f.set("pos:" + p.id, Math.round(p.metadata.x) + "," + Math.round(p.metadata.y));
                break;
            case "addedge":
                f.set("edge:" + end(p.src) + ">" + end(p.tgt), "yes");
                break;
            case "removeedge":
                f.set("edge:" + end(p.src) + ">" + end(p.tgt), null);
                break;
            case "addinitial":
                f.set("initial:" + end(p.tgt), String(p.src.data));
                break;
            case "removeinitial":
                f.set("initial:" + end(p.tgt), null);
                break;
            case "addinport":
            case "addoutport":
                f.set(dirOf(c.command) + "port:" + p.public, p.node + "." + p.port);
                break;
            case "removeinport":
            case "removeoutport":
                f.set(dirOf(c.command) + "port:" + p.public, null);
                break;
            case "renameinport":
            case "renameoutport":
                f.set(dirOf(c.command) + "port:" + p.from, null);
                f.set(dirOf(c.command) + "renamed:" + p.to, "yes");
                break;
            case "bind":
                f.set("binding:" + p.port + "|" + channel(p.env, p.index), "yes");
                break;
            case "unbind":
                f.set("binding:" + p.port + "|" + channel(p.env, p.index), null);
                break;
        }
    }
    return f;
}

// Whether the process is as the facts say.
export function holds(facts: Map<string, string | null>, s: StateReader): boolean {
    for (const [key, want] of facts) {
        const colon = key.indexOf(":");
        const kind = key.substring(0, colon), what = key.substring(colon + 1);
        let have: string | null = null;
        if (kind === "block")
            have = s.block(what)?.component ?? null;
        else if (kind === "exists")
            have = s.block(what) !== undefined ? "yes" : null;
        else if (kind === "pos") {
            const b = s.block(what);
            have = b === undefined ? null : Math.round(b.x) + "," + Math.round(b.y);
        } else if (kind === "edge") {
            const [src, tgt] = what.split(">");
            const t = splitEnd(tgt);
            const l = s.edgeInto(t.node, t.port);
            have = l !== undefined && l.src.node + "." + l.src.port === src ? "yes" : null;
        } else if (kind === "initial") {
            const e = splitEnd(what);
            have = s.initial(e.node, e.port) ?? null;
        } else if (kind === "inport" || kind === "outport") {
            const at = s.port(kind === "inport" ? "in" : "out", what);
            have = at === undefined ? null : at.node + "." + at.port;
        } else if (kind === "inrenamed" || kind === "outrenamed") {
            have = s.port(kind === "inrenamed" ? "in" : "out", what) !== undefined ? "yes" : null;
        } else if (kind === "binding") {
            const bar = what.lastIndexOf("|");
            have = s.bound(what.substring(0, bar), what.substring(bar + 1)) ? "yes" : null;
        }
        if (have !== want)
            return false;
    }
    return true;
}

function splitEnd(s: string): End {
    const dot = s.lastIndexOf(".");
    return {node: s.substring(0, dot), port: s.substring(dot + 1)};
}

// One process's undo and redo stacks, the oldest dropped beyond the size.
export interface History {
    undo: Step[];
    redo: Step[];
}

export function push(h: History, step: Step, size = HISTORY_SIZE) {
    h.undo.push(step);
    if (h.undo.length > size)
        h.undo.splice(0, h.undo.length - size);
    h.redo = [];
}
