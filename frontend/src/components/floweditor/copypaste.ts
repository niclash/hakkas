// Copy, paste and duplicate in the editor (06 §4.10). Ctrl+C and Ctrl+V go through the
// browser's copy and paste events, so the clip reaches the system clipboard and comes from it
// also on a plain-http page (navigator.clipboard needs a secure context); the page keeps the
// last clip too, for the context menu's Paste. Pasting sends commands; the pasted blocks become
// the selection as their events arrive.

import {can, PrimitiveNode, vueModel} from "@/components/model";
import {Clip, CLIP_FORMAT, clipOrigin, makeClip, parseClip, pasteCommands} from "@/components/clipboard";
import type {FlowEditor} from "./viewmodel/default";

// The offset of a pasted copy from its originals; each further paste of the same clip moves
// one step more.
const STEP = 40;

let clip: Clip | undefined;
let pastes = 0;
let expected = new Set<string>();   // the ids of pasted blocks not drawn yet

export function hasClip(): boolean {
    return clip !== undefined;
}

// The selected blocks as a clip, with the connections between them.
function selectionClip(editor: FlowEditor): Clip | undefined {
    const ids = editor.blockSelection?.selection.ids() ?? [];
    const blocks = ids.flatMap(id => {
        const node = editor.getNode(id);
        const at = editor.area.nodeViews.get(id)?.position;
        return node instanceof PrimitiveNode && at !== undefined
            ? [{id: node.label, component: node.component, x: at.x, y: at.y, initials: {...node.initials}}] : [];
    });
    if (blocks.length === 0)
        return undefined;
    const links = editor.getConnections().flatMap(c => {
        const s = editor.getNode(c.source), t = editor.getNode(c.target);
        return s instanceof PrimitiveNode && t instanceof PrimitiveNode
            ? [{src: {node: s.label, port: c.sourceOutput}, tgt: {node: t.label, port: c.targetInput}}] : [];
    });
    return makeClip(blocks, links);
}

function paste(editor: FlowEditor, c: Clip, dx: number, dy: number) {
    if (!can.value.edit)
        return;
    const graph = vueModel.currentGraphId.value;
    const proto = vueModel.connection.value?.proto.graph;
    if (graph === "" || proto === undefined)
        return;
    const taken = new Set(editor.getNodes().filter(n => n instanceof PrimitiveNode).map(n => n.label));
    const p = pasteCommands(c, graph, taken, dx, dy);
    editor.blockSelection?.selection.clear();
    expected = new Set(p.ids.values());
    for (const cmd of p.commands)
        proto.request_raw(cmd.command, cmd.payload);
}

export function copySelection(editor: FlowEditor): Clip | undefined {
    const c = selectionClip(editor);
    if (c !== undefined) {
        clip = c;
        pastes = 0;
    }
    return c;
}

// The page's clip, one step further than the last paste of it.
export function pasteClip(editor: FlowEditor, c: Clip | undefined = clip) {
    if (c === undefined)
        return;
    pastes++;
    paste(editor, c, STEP * pastes, STEP * pastes);
}

// The page's clip with its top-left corner at `at` (the context menu's Paste).
export function pasteClipAt(editor: FlowEditor, at: { x: number, y: number }) {
    if (clip === undefined)
        return;
    const origin = clipOrigin(clip);
    paste(editor, clip, at.x - origin.x, at.y - origin.y);
}

// The selection pasted beside itself, the clipboard untouched.
export function duplicateSelection(editor: FlowEditor) {
    const c = selectionClip(editor);
    if (c !== undefined)
        paste(editor, c, STEP, STEP);
}

// A block was drawn (graph:addnode): a pasted one joins the selection.
export function blockArrived(editor: FlowEditor, id: string, node: string) {
    if (expected.delete(id))
        editor.blockSelection?.selection.add([node]);
}

// Keys typed into a field are the field's.
function typing(event: Event): boolean {
    const target = event.target as HTMLElement | null;
    return target !== null && (target.isContentEditable || ["INPUT", "TEXTAREA", "SELECT"].includes(target.tagName));
}

export function setupCopyPaste(editor: FlowEditor) {
    const active = (event: Event) => editor.area.container.isConnected && !typing(event);
    const onCopy = (event: ClipboardEvent) => {
        if (!active(event))
            return;
        const c = copySelection(editor);
        if (c === undefined || event.clipboardData === null)
            return;
        event.preventDefault();
        const text = JSON.stringify(c);
        event.clipboardData.setData("text/plain", text);
        event.clipboardData.setData("application/x-" + CLIP_FORMAT.replace("/", "-"), text);
    };
    const onPaste = (event: ClipboardEvent) => {
        if (!active(event))
            return;
        const text = event.clipboardData?.getData("text/plain") ?? "";
        const c = parseClip(text);
        if (c === undefined)
            return;
        event.preventDefault();
        // The page's own clip again cascades; another one (another tab, engine) is new.
        if (clip === undefined || JSON.stringify(clip) !== JSON.stringify(c)) {
            clip = c;
            pastes = 0;
        }
        pasteClip(editor);
    };
    const onKey = (event: KeyboardEvent) => {
        if (!active(event) || event.key.toLowerCase() !== "d" || !(event.ctrlKey || event.metaKey) || event.altKey)
            return;
        event.preventDefault();   // the browser's bookmark
        duplicateSelection(editor);
    };
    document.addEventListener("copy", onCopy);
    document.addEventListener("paste", onPaste);
    document.addEventListener("keydown", onKey);
    return () => {
        document.removeEventListener("copy", onCopy);
        document.removeEventListener("paste", onPaste);
        document.removeEventListener("keydown", onKey);
    };
}
