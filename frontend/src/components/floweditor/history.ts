// Undo and redo in the editor (06 §4.13): every undoable graph command this tab sends is
// recorded with its inverse, in steps (one user action each: the commands sent together, or a
// drag between begin() and end()), per process, the 500 newest kept in sessionStorage (so a
// reload of the tab keeps them, and two tabs never share them). Ctrl+Z checks the process still
// is as the step left it, then sends its inverse; Ctrl+Shift+Z (or Ctrl+Y) does the reverse. A
// step changed by someone else meanwhile is skipped with a message.

import {can, isPortNode, PrimitiveNode, vueModel} from "@/components/model";
import {Connection, Packet} from "@/components/protocols/websocket";
import {Cmd, factsOf, History, holds, inverseOf, Link, makeStep, push, StateReader, Step, UNDOABLE} from "@/components/undo";
import type {FlowEditor} from "./viewmodel/default";

const STORAGE_KEY = "hakkas.undo";

let editor: FlowEditor | undefined;
let recording = true;
let depth = 0;
let group: { graph: string, cmds: Cmd[], inverses: Cmd[][] } | undefined;
let flushScheduled = false;
// A step sent and not yet seen applied: further undos and redos wait for it.
let waiting: { facts: Map<string, string | null>, timer: ReturnType<typeof setTimeout> } | undefined;
const queued: ("undo" | "redo")[] = [];
let tell: (text: string) => void = () => {};

let histories: Record<string, History> = load();

function load(): Record<string, History> {
    try {
        const v = JSON.parse(sessionStorage.getItem(STORAGE_KEY) ?? "{}");
        return v !== null && typeof v === "object" ? v : {};
    } catch {
        return {};
    }
}

function save() {
    try {
        sessionStorage.setItem(STORAGE_KEY, JSON.stringify(histories));
    } catch {
        // Storage full or unavailable: the history lives on in the page.
    }
}

function historyOf(graph: string): History {
    return histories[graph] ??= {undo: [], redo: []};
}

// The process as the editor shows it.
const reader: StateReader = {
    block(id) {
        const n = findBlock(id);
        const at = n && editor?.area.nodeViews.get(n.id)?.position;
        return n && at ? {component: n.component, x: at.x, y: at.y} : undefined;
    },
    initial: (node, port) => findBlock(node)?.initials[port],
    initials: node => ({...(findBlock(node)?.initials ?? {})}),
    edges: node => links().filter(l => l.src.node === node || l.tgt.node === node),
    edgeInto: (node, port) => links().find(l => l.tgt.node === node && l.tgt.port === port),
    ports(node) {
        const b = findBlock(node);
        return b === undefined || editor === undefined ? [] : editor.getNodes().filter(isPortNode)
            .filter(p => p.block === b.id).map(p => ({direction: p.direction, name: p.name, port: p.port}));
    },
    port(direction, name) {
        const p = editor?.getNodes().filter(isPortNode).find(p => p.direction === direction && p.name === name);
        const b = p && editor?.getNode(p.block);
        return p && b instanceof PrimitiveNode ? {node: b.label, port: p.port} : undefined;
    },
};

function findBlock(id: string): PrimitiveNode | undefined {
    return editor?.getNodes().find((n): n is PrimitiveNode => n instanceof PrimitiveNode && n.label === id);
}

function links(): Link[] {
    const e = editor;
    if (e === undefined)
        return [];
    return e.getConnections().flatMap(c => {
        const s = e.getNode(c.source), t = e.getNode(c.target);
        return s instanceof PrimitiveNode && t instanceof PrimitiveNode
            ? [{src: {node: s.label, port: c.sourceOutput}, tgt: {node: t.label, port: c.targetInput}}] : [];
    });
}

// ---- Recording --------------------------------------------------------------------------

function record(packet: Packet) {
    const payload = packet.payload as Record<string, any> | undefined;
    if (!recording || packet.protocol !== "graph" || !UNDOABLE.has(packet.command) || payload?.graph === undefined)
        return;
    const cmd: Cmd = {command: packet.command, payload: JSON.parse(JSON.stringify(payload))};
    if (group !== undefined && group.graph !== payload.graph)
        flush();
    group ??= {graph: payload.graph, cmds: [], inverses: []};
    group.cmds.push(cmd);
    // The process before the command: its own event has not arrived yet.
    group.inverses.push(payload.graph === vueModel.currentGraphId.value ? inverseOf(cmd, reader) : []);
    if (depth === 0 && !flushScheduled) {
        flushScheduled = true;
        queueMicrotask(() => {
            flushScheduled = false;
            if (depth === 0)
                flush();
        });
    }
}

function flush() {
    const g = group;
    group = undefined;
    if (g === undefined || g.cmds.length === 0)
        return;
    // A command whose inverse could not be known (another process than the open one) makes the
    // step one that cannot be undone.
    if (g.inverses.some((inv, i) => inv.length === 0 && g.cmds[i].command !== "changenode"))
        return;
    push(historyOf(g.graph), makeStep(g.graph, g.cmds, g.inverses));
    save();
}

// A user action made of commands sent over time (a drag): one step.
export function begin() {
    depth++;
}

export function end() {
    if (depth > 0 && --depth === 0)
        flush();
}

// Commands sent while fn runs are not recorded (e.g. building a composite type's graph).
export function unrecorded(fn: () => void) {
    recording = false;
    try {
        fn();
    } finally {
        recording = true;
    }
}

// ---- Undoing and redoing ----------------------------------------------------------------

function send(cmds: Cmd[]) {
    const proto = vueModel.connection.value?.proto.graph;
    unrecorded(() => cmds.forEach(c => proto?.request_raw(c.command, c.payload)));
}

function apply(direction: "undo" | "redo") {
    const graph = vueModel.currentGraphId.value;
    if (graph === "" || !can.value.edit)
        return;
    if (waiting !== undefined) {
        queued.push(direction);
        return;
    }
    flush();
    const h = historyOf(graph);
    const step: Step | undefined = (direction === "undo" ? h.undo : h.redo).pop();
    if (step === undefined)
        return;
    const expect = factsOf(direction === "undo" ? step.forward : step.inverse);
    if (!holds(expect, reader)) {
        save();
        tell(direction === "undo" ? "Not undone: changed since by someone else" : "Not redone: changed since by someone else");
        return;
    }
    const cmds = direction === "undo" ? step.inverse : step.forward;
    (direction === "undo" ? h.redo : h.undo).push(step);
    save();
    send(cmds);
    waiting = {facts: factsOf(cmds), timer: setTimeout(settled, 3000)};
}

// The step's events are in (or it took too long, e.g. a part was refused): the next may go.
function settled() {
    if (waiting === undefined)
        return;
    clearTimeout(waiting.timer);
    waiting = undefined;
    const next = queued.shift();
    if (next !== undefined)
        apply(next);
}

// Called after each graph event is applied to the editor.
export function eventApplied() {
    if (waiting !== undefined && holds(waiting.facts, reader))
        settled();
}

export function undo() {
    apply("undo");
}

export function redo() {
    apply("redo");
}

// Keys typed into a field are the field's.
function typing(event: Event): boolean {
    const target = event.target as HTMLElement | null;
    return target !== null && (target.isContentEditable || ["INPUT", "TEXTAREA", "SELECT"].includes(target.tagName));
}

export function setupHistory(e: FlowEditor, notify: (text: string) => void) {
    editor = e;
    tell = notify;
    Connection.sendHook = record;
    const onKey = (event: KeyboardEvent) => {
        if (!e.area.container.isConnected || typing(event) || !(event.ctrlKey || event.metaKey) || event.altKey)
            return;
        const key = event.key.toLowerCase();
        if (key === "z" && !event.shiftKey) {
            event.preventDefault();
            undo();
        } else if ((key === "z" && event.shiftKey) || key === "y") {
            event.preventDefault();
            redo();
        }
    };
    document.addEventListener("keydown", onKey);
    return () => document.removeEventListener("keydown", onKey);
}
