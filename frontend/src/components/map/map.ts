// The process map (06 §4.17, plan task 79): its state, fed by one connection's link:links,
// link:add/remove, process port events (system-wide, 02 §4.1) and new and deleted processes;
// opening and closing it; and the map's own undo history (06 §4.13). The view is MapView.vue.

import {reactive, ref} from "vue";
import {Connection} from "@/components/protocols/websocket";
import type {ProcessLink, ProcessPorts} from "@/components/protocols/link";
import {can, vueModel} from "@/components/model";
import {refreshBindings} from "@/components/environment";
import {
    addLink, addPort, addProcess, emptyMap, loadLinks, MapStep, removeLink, removePort, removeProcess, renamePort,
    stepHolds
} from "./state";

// The map's state; `version` changes with every applied message, for the view to follow.
export const processMap = reactive({state: emptyMap(), version: 0, loaded: false});
export const mapOpen = ref(false);

const changed = () => processMap.version++;

// Feeds the map from a connection (called for every connection, before its login).
export function attachMap(conn: Connection) {
    processMap.state = emptyMap();
    processMap.loaded = false;
    changed();
    const link = conn.proto.link, graph = conn.proto.graph, runtime = conn.proto.runtime;
    link.addListener("onLinks", (p: { links: ProcessLink[], processes: ProcessPorts[] }) => {
        loadLinks(processMap.state, p.links ?? [], p.processes ?? []);
        processMap.loaded = true;
        changed();
    });
    link.addListener("onAdd", (l: ProcessLink) => {
        addLink(processMap.state, l);
        changed();
        settled();
    });
    link.addListener("onRemove", (l: ProcessLink) => {
        removeLink(processMap.state, l);
        changed();
        settled();
    });
    // The port events of every process (system-wide since plan task 79, 02 §4.1).
    const port = (apply: (p: any) => void) => (p: any) => {
        apply(p);
        changed();
    };
    graph.addListener("onGraphAddInport", port(p => addPort(processMap.state, p.graph, "in", p.name)));
    graph.addListener("onGraphAddOutport", port(p => addPort(processMap.state, p.graph, "out", p.name)));
    graph.addListener("onGraphRemoveInport", port(p => removePort(processMap.state, p.graph, "in", p.name)));
    graph.addListener("onGraphRemoveOutport", port(p => removePort(processMap.state, p.graph, "out", p.name)));
    graph.addListener("onGraphRenameInport", port(p => renamePort(processMap.state, p.graph, "in", p.from, p.to)));
    graph.addListener("onGraphRenameOutport", port(p => renamePort(processMap.state, p.graph, "out", p.from, p.to)));
    runtime.addListener("onNewGraphPacket", (p: { graph: string }) => {
        addProcess(processMap.state, p.graph);
        changed();
    });
    runtime.addListener("onDeleteGraphPacket", (p: { graph: string }) => {
        removeProcess(processMap.state, p.graph);
        changed();
    });
    // Opening a process leaves the map.
    graph.addListener("onGraphClear", () => {
        mapOpen.value = false;
    });
}

// Shows the map: its links and processes read again, and the bindings for the ports' channels.
export function openMap(conn: Connection | undefined = vueModel.connection.value) {
    mapOpen.value = true;
    if (conn === undefined)
        return;
    conn.proto.link.request_list();
    void refreshBindings(conn);
}

// ---- Making and removing links, and undoing it ----------------------------------------------

const STORAGE_KEY = "hakkas.undo.map";
const HISTORY_SIZE = 500;
let history: { undo: MapStep[], redo: MapStep[] } = load();
// A step sent and not yet seen applied: the next undo or redo waits for it (at most 3 s).
let waiting: ReturnType<typeof setTimeout> | undefined;
let tell: (text: string) => void = () => {};

function load() {
    try {
        const v = JSON.parse(sessionStorage.getItem(STORAGE_KEY) ?? "{}");
        return Array.isArray(v?.undo) && Array.isArray(v?.redo) ? v : {undo: [], redo: []};
    } catch {
        return {undo: [], redo: []};
    }
}

function save() {
    try {
        sessionStorage.setItem(STORAGE_KEY, JSON.stringify(history));
    } catch {
        // Storage full or unavailable: the history lives on in the page.
    }
}

function send(step: MapStep, forward: boolean) {
    const proto = vueModel.connection.value?.proto.link;
    if (step.add === forward)
        proto?.request_add(step.link);
    else
        proto?.request_remove(step.link);
}

// A link made or removed by the user: sent, and recorded as a step.
export function changeLink(add: boolean, link: ProcessLink) {
    if (!can.value.edit)
        return;
    const step: MapStep = {add, link: {src: {...link.src}, tgt: {...link.tgt}}};
    history.undo.push(step);
    if (history.undo.length > HISTORY_SIZE)
        history.undo.shift();
    history.redo = [];
    save();
    send(step, true);
}

function settled() {
    clearTimeout(waiting);
    waiting = undefined;
}

function apply(undoing: boolean) {
    if (!can.value.edit || waiting !== undefined)
        return;
    const step = (undoing ? history.undo : history.redo).pop();
    if (step === undefined)
        return;
    if (!stepHolds(processMap.state, step, undoing)) {
        save();
        tell(undoing ? "Not undone: changed since by someone else" : "Not redone: changed since by someone else");
        return;
    }
    (undoing ? history.redo : history.undo).push(step);
    save();
    send(step, !undoing);
    waiting = setTimeout(settled, 3000);
}

export const undoLink = () => apply(true);
export const redoLink = () => apply(false);

// Where the map's "Not undone" notices go.
export function setMapNotice(fn: (text: string) => void) {
    tell = fn;
}

// Opens a process from the map (a double-click, its menu): the map gives way to the editor.
export function openProcess(graph: string) {
    mapOpen.value = false;
    vueModel.connection.value?.proto.graph.request_connect(graph);
}
