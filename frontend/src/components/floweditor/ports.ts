// Process ports drawn as markers (06 §4.8): a PortNode per inport and outport, connected to its
// block port and placed at the process's edge. Called from the graph event handlers, so every
// change here follows an engine event.

import {Connection, isPortNode, isValueNode, PortNode, PrimitiveNode} from "@/components/model";
import type {FlowEditor} from "./viewmodel/default";
import {Box, layoutPorts} from "./port-layout";
import {socketOffset} from "./values";

export function portNodes(editor: FlowEditor): PortNode[] {
    return editor.getNodes().filter(isPortNode);
}

function portNode(editor: FlowEditor, direction: "in" | "out", name: string): PortNode | undefined {
    return portNodes(editor).find(p => p.direction === direction && p.name === name);
}

export async function showPort(editor: FlowEditor, direction: "in" | "out", name: string, block: PrimitiveNode, port: string) {
    const marker = new PortNode(direction, name, block, port);
    await editor.addNode(marker);
    const line = direction === "in"
        ? new Connection(marker, "port" as never, block, port as never)
        : new Connection(block, port as never, marker, "port" as never);
    await editor.addConnection(line as never);
    await placePorts(editor);
}

export async function removePort(editor: FlowEditor, direction: "in" | "out", name: string) {
    const marker = portNode(editor, direction, name);
    if (marker === undefined)
        return;
    for (const c of editor.getConnections().filter(c => c.source === marker.id || c.target === marker.id))
        await editor.removeConnection(c.id);
    await editor.removeNode(marker.id);
    await placePorts(editor);
}

export async function renamePort(editor: FlowEditor, direction: "in" | "out", from: string, to: string) {
    const marker = portNode(editor, direction, from);
    if (marker === undefined)
        return;
    marker.name = to;
    marker.label = to;
    await editor.area.update("node", marker.id);
}

// Puts every marker at the process's edge, level with its block port.
export async function placePorts(editor: FlowEditor) {
    const markers = portNodes(editor);
    if (markers.length === 0)
        return;
    const content: Box[] = [];
    for (const node of editor.getNodes()) {
        const view = editor.area.nodeViews.get(node.id);
        if (view !== undefined && (node instanceof PrimitiveNode || isValueNode(node)))
            content.push({x: view.position.x, y: view.position.y, width: node.width, height: node.height});
    }
    const placed = layoutPorts(content, markers.flatMap(m => {
        const block = editor.getNode(m.block);
        const view = editor.area.nodeViews.get(m.block);
        if (!(block instanceof PrimitiveNode) || view === undefined)
            return [];
        const y = view.position.y + socketOffset(editor, block, m.port, m.direction === "in" ? "input" : "output");
        return [{id: m.id, direction: m.direction, socketY: y, width: m.width, height: m.height}];
    }));
    for (const [id, at] of placed)
        await editor.area.translate(id, at);
}
