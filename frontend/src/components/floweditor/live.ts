// Live values (06 §4.7): the open process's watched inputs, sent to the engine as network:edges,
// and the values its network:data brings. The views (connection labels, the inspector) read
// `live`; the graph event handlers keep the watches in step with the process.

import {reactive} from "vue";
import {NodeEditor} from "rete";
import {PrimitiveNode, Schemes, vueModel} from "@/components/model";
import {Point, pointKey, Watch, Watches} from "@/components/watches";

export const live = reactive({
    // The latest value of each watched input, by pointKey(tgt); `stale` once the connection to
    // the engine is lost.
    values: new Map<string, { data: string, stale: boolean }>(),
    // The inputs in the set last sent, by pointKey(tgt).
    watched: new Set<string>(),
    all: false,
    // Incremented when a block is renamed: the views look up their input's value by the
    // block's id, which is not reactive.
    version: 0,
});

const watches = new Watches();
let graph = "";
let editor: NodeEditor<Schemes> | undefined;
let timer: ReturnType<typeof setTimeout> | undefined;

export function attachLive(e: NodeEditor<Schemes>) {
    editor = e;
}

// The process's connections (not the value nodes' lines) as watches.
function connections(): Watch[] {
    if (editor === undefined)
        return [];
    const e = editor;
    return e.getConnections().flatMap(c => {
        const source = e.getNode(c.source);
        const target = e.getNode(c.target);
        if (!(source instanceof PrimitiveNode) || !(target instanceof PrimitiveNode))
            return [];
        return [{src: {node: source.label, port: c.sourceOutput}, tgt: {node: target.label, port: c.targetInput}}];
    });
}

function send() {
    clearTimeout(timer);
    timer = undefined;
    const list = graph === "" ? [] : watches.request(connections());
    live.watched = new Set(list.map(w => pointKey(w.tgt)));
    live.all = watches.all;
    for (const key of [...live.values.keys()]) {
        if (!live.watched.has(key))
            live.values.delete(key);
    }
    vueModel.connection.value?.proto.network.request_edges(list as never);
}

// Sends the set once the events of a burst (e.g. a snapshot) have been applied.
function schedule() {
    if (timer === undefined)
        timer = setTimeout(send, 50);
}

// What feeds an input now: its connection's output, or the input itself.
function watchOf(tgt: Point): Watch {
    return connections().find(c => pointKey(c.tgt) === pointKey(tgt)) ?? {src: tgt, tgt};
}

export function isWatched(tgt: Point): boolean {
    return live.watched.has(pointKey(tgt));
}

export function watch(tgts: Point[]) {
    tgts.forEach(t => watches.add(watchOf(t)));
    send();
}

// An input watched only through "watch all" is unwatched by turning that off.
export function unwatch(tgts: Point[]) {
    if (watches.all && tgts.some(t => !watches.has(t) && isWatched(t)))
        watches.all = false;
    tgts.forEach(t => watches.remove(t));
    send();
}

export function watchAll(on: boolean) {
    watches.all = on;
    send();
}

// A block's inputs, as points.
export function inputsOf(block: PrimitiveNode): Point[] {
    return Object.keys(block.inputs).map(port => ({node: block.label, port}));
}

export function liveValue(tgt: Point): { data: string, stale: boolean } | undefined {
    return live.values.get(pointKey(tgt));
}

// --- From the events -------------------------------------------------------------------------

// graph:clear: another process clears the watches (the engine's set is replaced by an empty
// one); the same process again (reopened, or after a reconnect) gets them sent again.
export function processOpened(id: string) {
    live.values.clear();
    if (id !== graph) {
        const had = live.watched.size > 0;
        watches.clear();
        graph = id;
        if (had)
            send();
        return;
    }
    schedule();
}

export function blockRenamed(from: string, to: string) {
    watches.renameBlock(from, to);
    live.version++;
    schedule();
}

export function blockRemoved(id: string) {
    watches.removeBlock(id);
    schedule();
}

// A connection added or removed: "watch all" and the watched inputs follow it.
export function connectionsChanged() {
    if (watches.all || live.watched.size > 0)
        schedule();
}

export function dataArrived(payload: { graph: string, tgt: Point, data: unknown }) {
    if (payload.graph !== graph || !isWatched(payload.tgt))
        return;
    live.values.set(pointKey(payload.tgt), {data: String(payload.data), stale: false});
}

// The connection to the engine is lost: the values shown are no longer current.
export function connectionLost() {
    for (const [key, value] of live.values)
        live.values.set(key, {...value, stale: true});
}

