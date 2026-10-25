// Fixed values drawn as value nodes (06 §4.3): each initial is a ValueNode connected to its
// input, placed just left of the input and moved with its block. Called from the graph event
// handlers, so every change here follows an engine event.

import {NodeEditor} from "rete";
import {Connection, isPortNode, isValueNode, PrimitiveNode, Schemes, ValueNode, vueModel} from "@/components/model";
import {bindingsAt} from "@/components/environment";
import {channelLabel} from "@/components/bindings";
import {InputSource} from "@/components/initials";
import type {FlowEditor} from "./viewmodel/default";
import {Position, valuePosition} from "./value-position";

export function valueNodes(editor: FlowEditor, block?: PrimitiveNode): ValueNode[] {
    return editor.getNodes().filter(isValueNode).filter(v => block === undefined || v.block === block.id);
}

function valueNodeOf(editor: FlowEditor, block: PrimitiveNode, port: string): ValueNode | undefined {
    return valueNodes(editor, block).find(v => v.port === port);
}

// The middle of a port's socket below the block's top, as drawn (without the zoom).
export function socketOffset(editor: FlowEditor, block: PrimitiveNode, port: string,
                             side: "input" | "output" = "input"): number {
    const view = editor.area.nodeViews.get(block.id);
    const socket = view?.element.querySelector(
        `[data-testid="${side}-${CSS.escape(port)}"] [data-testid="${side}-socket"]`);
    if (view !== undefined && socket !== null && socket !== undefined) {
        const top = view.element.getBoundingClientRect().top;
        const rect = socket.getBoundingClientRect();
        if (rect.height > 0)
            return (rect.top + rect.height / 2 - top) / editor.area.area.transform.k;
    }
    // Not drawn yet: the classic block's layout (a title, then outputs and inputs in rows).
    const row = side === "output" ? Object.keys(block.outputs).indexOf(port)
        : Object.keys(block.outputs).length + Object.keys(block.inputs).indexOf(port);
    return 40 + row * 36 + 18;
}

async function place(editor: FlowEditor, value: ValueNode, block: PrimitiveNode) {
    const view = editor.area.nodeViews.get(block.id);
    if (view === undefined)
        return;
    const at: Position = valuePosition(view.position, socketOffset(editor, block, value.port), value);
    await editor.area.translate(value.id, at);
}

// Puts the block's value nodes beside their inputs (after the block moved or was drawn).
export async function placeValues(editor: FlowEditor, block: PrimitiveNode) {
    for (const value of valueNodes(editor, block))
        await place(editor, value, block);
}

// Shows the input's fixed value (graph:addinitial), or removes it (undefined,
// graph:removeinitial).
export async function showValue(editor: FlowEditor, block: PrimitiveNode, port: string, data: string | undefined) {
    block.setInitial(port, data);
    const existing = valueNodeOf(editor, block, port);
    if (data === undefined) {
        if (existing !== undefined) {
            for (const c of editor.getConnections().filter(c => c.source === existing.id))
                await editor.removeConnection(c.id);
            await editor.removeNode(existing.id);
        }
        return;
    }
    if (existing !== undefined) {
        existing.value = data;
        await editor.area.update("node", existing.id);
        return;
    }
    const value = new ValueNode(block, port, data);
    await editor.addNode(value);
    await editor.addConnection(new Connection(value, "value" as never, block, port as never) as never);
    await place(editor, value, block);
}

// The block was renamed: its value nodes send commands with the new id.
export async function renameValues(editor: FlowEditor, block: PrimitiveNode) {
    for (const value of valueNodes(editor, block)) {
        value.blockLabel = block.label;
        await editor.area.update("node", value.id);
    }
}

// What feeds each of the block's inputs that has a connection, a fixed value or an environment
// sensor (through a bound process inport, 06 §4.14).
export function inputSources(editor: NodeEditor<Schemes>, block: PrimitiveNode): Record<string, InputSource> {
    const inputs: Record<string, InputSource> = {};
    for (const [port, value] of Object.entries(block.initials))
        inputs[port] = {kind: "fixed", value};
    for (const c of editor.getConnections()) {
        const source = editor.getNode(c.source);
        if (c.target === block.id && source instanceof PrimitiveNode)
            inputs[c.targetInput] = {kind: "connected", from: source.label + "." + c.sourceOutput};
        else if (c.target === block.id && isPortNode(source)) {
            const b = bindingsAt(vueModel.currentGraphId.value, "in", source.name)[0];
            if (b !== undefined)
                inputs[c.targetInput] = {kind: "bound", channel: channelLabel(b.env, b.index), port: source.name};
        }
    }
    return inputs;
}
