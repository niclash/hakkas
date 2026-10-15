// Creating a composite block type from the selected blocks (06 §4.9, 02 §3.4.3): the port
// mapping the dialog starts from, its checks, and the commands that build the type's component
// graph. Kept free of Vue and Rete so that it can be unit tested.

export interface SelPort {
    port: string;
    type: string;
}

export interface SelBlock {
    id: string;
    component: string;
    x: number;
    y: number;
    inputs: SelPort[];
    outputs: SelPort[];
    initials: Record<string, string>;   // fixed values, as the events carry them
}

export interface Link {
    src: { node: string, port: string };
    tgt: { node: string, port: string };
}

export interface ProcessPort {
    direction: "in" | "out";
    name: string;
    node: string;
    port: string;
}

// One block port in the dialog: exposed or not, under which public name.
export interface PortRow {
    direction: "in" | "out";
    node: string;
    port: string;
    type: string;
    exposed: boolean;
    name: string;
    // Why it is exposed at first: a connection crossing the selection, or a process port.
    why?: "connection" | "process-port";
    fixed?: string;     // an exposed input's fixed value is not part of the type
}

const MAX_NAME = 64;

function valid(name: string): boolean {
    return name !== "" && new TextEncoder().encode(name).length <= MAX_NAME && !name.includes(".");
}

// A free name for node.port among taken: the port's own, else "<node>_<port>", else
// "<port>_<n>" with the smallest n >= 2 (03 §5.2's default names).
export function suggestName(taken: Set<string>, node: string, port: string): string {
    if (valid(port) && !taken.has(port))
        return port;
    const both = node + "_" + port;
    if (valid(both) && !taken.has(both))
        return both;
    const base = valid(port + "_2") ? port : "port";
    for (let n = 2; ; n++) {
        if (!taken.has(base + "_" + n))
            return base + "_" + n;
    }
}

// The rows the dialog starts with: every input, then every output, of the selected blocks, in
// their order. Exposed at first, as 03 §5.2 does: an input fed by a connection from outside the
// selection, an output with a connection to outside it, and a block port that is a process port
// (under its public name).
export function defaultPorts(blocks: SelBlock[], links: Link[], processPorts: ProcessPort[]): PortRow[] {
    const inside = new Set(blocks.map(b => b.id));
    const rows: PortRow[] = [];
    for (const direction of ["in", "out"] as const) {
        const taken = new Set<string>();
        const column: PortRow[] = [];
        for (const b of blocks) {
            for (const p of direction === "in" ? b.inputs : b.outputs) {
                const pp = processPorts.find(q => q.direction === direction && q.node === b.id && q.port === p.port);
                const crossing = links.some(l => direction === "in"
                    ? l.tgt.node === b.id && l.tgt.port === p.port && !inside.has(l.src.node)
                    : l.src.node === b.id && l.src.port === p.port && !inside.has(l.tgt.node));
                const row: PortRow = {direction, node: b.id, port: p.port, type: p.type, exposed: false, name: ""};
                if (pp !== undefined && !taken.has(pp.name)) {
                    row.exposed = true;
                    row.why = "process-port";
                    row.name = pp.name;
                    taken.add(pp.name);
                } else if (crossing) {
                    row.exposed = true;
                    row.why = "connection";
                }
                if (direction === "in" && b.initials[p.port] !== undefined)
                    row.fixed = b.initials[p.port];
                column.push(row);
            }
        }
        // Names: the exposed ones first — a port name that two of them share, or that a public
        // name has, goes to none of them: each is "<block>_<port>" (03 §5.2) — then a
        // suggestion for each of the others.
        const unnamed = column.filter(r => r.exposed && r.name === "");
        for (const row of unnamed) {
            const shared = unnamed.filter(r => r.port === row.port).length > 1 || taken.has(row.port);
            row.name = shared ? suggestName(new Set([...taken, row.port]), row.node, row.port)
                : suggestName(taken, row.node, row.port);
        }
        unnamed.forEach(r => taken.add(r.name));
        for (const row of column.filter(r => !r.exposed))
            taken.add(row.name = suggestName(taken, row.node, row.port));
        rows.push(...column);
    }
    return rows;
}

// A type name is "<family>/<name>", each part letters, digits, "-" and "_" (06 §4.9).
const TYPE_NAME = /^[A-Za-z0-9_-]+\/[A-Za-z0-9_-]+$/;

// What is wrong with the type's name or a row's public name, or "" (the engine checks too).
export function checkTypeName(name: string, existing: string[]): string {
    if (!TYPE_NAME.test(name))
        return "“family/Name”: letters, digits, “-” and “_” on each side of one “/”";
    if (name.length > MAX_NAME)
        return "A name of 1 to 64 characters";
    if (existing.includes(name))
        return "A block type with this name exists";
    return "";
}

export function checkPorts(rows: PortRow[]): Map<number, string> {
    const problems = new Map<number, string>();
    for (const direction of ["in", "out"] as const) {
        const seen = new Map<string, number>();
        rows.forEach((row, i) => {
            if (row.direction !== direction || !row.exposed)
                return;
            if (!valid(row.name))
                problems.set(i, "1 to 64 characters, no “.”");
            else if (seen.has(row.name)) {
                problems.set(i, "Used twice");
                problems.set(seen.get(row.name)!, "Used twice");
            } else
                seen.set(row.name, i);
        });
    }
    return problems;
}

export interface Command {
    protocol: "graph" | "network";
    command: string;
    payload: Record<string, unknown>;
}

// The commands that make the type: its component graph (graph:clear, main false), the selected
// blocks where they are, the connections between them, their fixed values except on exposed
// inputs, the exposed ports, and network:persist, which makes it the type.
export function componentCommands(type: string, label: string, description: string, blocks: SelBlock[],
                                  links: Link[], rows: PortRow[]): Command[] {
    const graph = type;
    const inside = new Set(blocks.map(b => b.id));
    const exposedInputs = new Set(rows.filter(r => r.direction === "in" && r.exposed).map(r => r.node + "\u0000" + r.port));
    const cmds: Command[] = [{
        protocol: "graph", command: "clear",
        payload: {id: type, name: label, library: type.split("/")[0], main: false, description, icon: ""},
    }];
    for (const b of blocks)
        cmds.push({protocol: "graph", command: "addnode", payload: {graph, id: b.id, component: b.component, metadata: {x: b.x, y: b.y}}});
    for (const l of links) {
        if (inside.has(l.src.node) && inside.has(l.tgt.node))
            cmds.push({protocol: "graph", command: "addedge", payload: {graph, src: l.src, tgt: l.tgt}});
    }
    for (const b of blocks) {
        for (const [port, data] of Object.entries(b.initials)) {
            if (!exposedInputs.has(b.id + "\u0000" + port))
                cmds.push({protocol: "graph", command: "addinitial", payload: {graph, tgt: {node: b.id, port}, src: {data}}});
        }
    }
    for (const r of rows.filter(r => r.exposed)) {
        cmds.push({
            protocol: "graph", command: r.direction === "in" ? "addinport" : "addoutport",
            payload: {graph, node: r.node, port: r.port, public: r.name},
        });
    }
    cmds.push({protocol: "network", command: "persist", payload: {graph}});
    return cmds;
}
