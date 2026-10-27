// The process map's model (06 §4.17, plan task 79): every process's ports and every link
// between processes, fed only by the engine's messages (02 §1.1). Kept free of Vue and Rete so
// that it can be unit tested. Applying a message is idempotent: a link or port that is already
// there (or already gone) changes nothing.

import type {ProcessLink, ProcessPorts} from "@/components/protocols/link";

export interface MapState {
    processes: Map<string, { inports: string[], outports: string[] }>;
    links: ProcessLink[];
}

export const emptyMap = (): MapState => ({processes: new Map(), links: []});

export const sameLink = (a: ProcessLink, b: ProcessLink) =>
    a.src.graph === b.src.graph && a.src.port === b.src.port && a.tgt.graph === b.tgt.graph && a.tgt.port === b.tgt.port;

// A link as a key, for the view's connection ids.
export const linkKey = (l: ProcessLink) => JSON.stringify([l.src.graph, l.src.port, l.tgt.graph, l.tgt.port]);

// link:links: everything, replacing what was there.
export function loadLinks(s: MapState, links: ProcessLink[], processes: ProcessPorts[]) {
    s.processes = new Map(processes.map(p => [p.graph, {inports: [...p.inports], outports: [...p.outports]}]));
    s.links = links.map(l => ({src: {...l.src}, tgt: {...l.tgt}}));
}

export function addLink(s: MapState, l: ProcessLink) {
    if (!s.links.some(x => sameLink(x, l)))
        s.links.push({src: {...l.src}, tgt: {...l.tgt}});
}

export function removeLink(s: MapState, l: ProcessLink) {
    s.links = s.links.filter(x => !sameLink(x, l));
}

// A new process has no ports yet.
export function addProcess(s: MapState, graph: string) {
    if (!s.processes.has(graph))
        s.processes.set(graph, {inports: [], outports: []});
}

// A deleted process: the engine reports its links' removal first, but a link to it can go too.
export function removeProcess(s: MapState, graph: string) {
    s.processes.delete(graph);
    s.links = s.links.filter(l => l.src.graph !== graph && l.tgt.graph !== graph);
}

const listOf = (s: MapState, graph: string, dir: "in" | "out") => {
    const p = s.processes.get(graph);
    return p === undefined ? undefined : dir === "in" ? p.inports : p.outports;
};

// A port event of a process the map knows (a component graph's are not processes').
export function addPort(s: MapState, graph: string, dir: "in" | "out", name: string) {
    const list = listOf(s, graph, dir);
    if (list !== undefined && !list.includes(name))
        list.push(name);
}

export function removePort(s: MapState, graph: string, dir: "in" | "out", name: string) {
    const list = listOf(s, graph, dir);
    const i = list?.indexOf(name) ?? -1;
    if (i >= 0)
        list!.splice(i, 1);
}

export function renamePort(s: MapState, graph: string, dir: "in" | "out", from: string, to: string) {
    const list = listOf(s, graph, dir);
    const i = list?.indexOf(from) ?? -1;
    if (i >= 0 && !list!.includes(to))
        list![i] = to;
}

// ---- Undo (06 §4.13, §4.17): the map's own steps ----------------------------------------------

// A step: a link made (add) or removed by this tab.
export interface MapStep {
    add: boolean;
    link: ProcessLink;
}

// Whether the map still is as the step left it (the link there after an add, gone after a
// remove), so that undoing it is this tab's own change; redo the reverse.
export const stepHolds = (s: MapState, step: MapStep, undoing: boolean) =>
    s.links.some(l => sameLink(l, step.link)) === (step.add === undoing);

// ---- Layout (06 §4.17): columns by the links, left to right -----------------------------------

export interface Size {
    width: number;
    height: number;
}

const COLUMN_GAP = 160, ROW_GAP = 48;

// Where each process goes: a process fed by no other process in the first column, every other one
// right of the processes that feed it (the longest chain of links; a loop of links is cut where
// it closes); within a column in the processes' order, top to bottom. Columns are as wide as
// their widest process.
export function layoutMap(s: MapState, size: (graph: string) => Size): Map<string, { x: number, y: number }> {
    const graphs = [...s.processes.keys()];
    const feeders = new Map(graphs.map(g => [g, new Set<string>()]));
    for (const l of s.links)
        if (l.src.graph !== l.tgt.graph && feeders.has(l.src.graph))
            feeders.get(l.tgt.graph)?.add(l.src.graph);
    const column = new Map<string, number>();
    const visiting = new Set<string>();
    const columnOf = (g: string): number => {
        const known = column.get(g);
        if (known !== undefined)
            return known;
        if (visiting.has(g))
            return -1;                      // a loop: closes here
        visiting.add(g);
        let c = 0;
        for (const f of feeders.get(g) ?? [])
            c = Math.max(c, columnOf(f) + 1);
        visiting.delete(g);
        column.set(g, c);
        return c;
    };
    graphs.forEach(columnOf);
    const columns: string[][] = [];
    for (const g of graphs)
        (columns[column.get(g)!] ??= []).push(g);
    const at = new Map<string, { x: number, y: number }>();
    let x = 0;
    for (const members of columns) {
        if (members === undefined)
            continue;
        let y = 0, width = 0;
        for (const g of members) {
            const sz = size(g);
            at.set(g, {x, y});
            y += sz.height + ROW_GAP;
            width = Math.max(width, sz.width);
        }
        x += width + COLUMN_GAP;
    }
    return at;
}
