// What the inspector panel shows for the selected blocks (06 §4.2), from the blocks' types
// (component:component). Kept free of Vue and Rete so that it can be unit tested.

import {ComponentEvent} from "@/components/protocols/component";
import {Port} from "@/components/protocols/protocols";

// A selected block: its id in the process and its type; `node` is the editor's own id for
// it, which a rename keeps.
export interface SelectedBlock {
    node?: string;
    id: string;
    component: string;
}

export interface PortView {
    id: string;
    type: string;
    description: string;
    default?: string;      // shown as the event carries it
}

export type InspectorView =
    | { kind: "none" }
    | {
    kind: "one", id: string, component: string, description: string,
    known: boolean,        // false for a placeholder (05 §2.2): its type is not registered
    inputs: PortView[], outputs: PortView[]
}
    | { kind: "many", ids: string[] };

function portView(port: Port): PortView {
    const view: PortView = {id: port.id, type: port.type ?? "", description: port.description ?? ""};
    if (port.default !== undefined && port.default !== null)
        view.default = String(port.default);
    return view;
}

export function inspect(blocks: SelectedBlock[], types: Record<string, ComponentEvent>): InspectorView {
    if (blocks.length === 0)
        return {kind: "none"};
    if (blocks.length > 1)
        return {kind: "many", ids: blocks.map(b => b.id).sort()};
    const block = blocks[0];
    const type = types[block.component];
    if (type === undefined)
        return {kind: "one", id: block.id, component: block.component, description: "", known: false, inputs: [], outputs: []};
    return {
        kind: "one", id: block.id, component: block.component, description: type.description ?? "", known: true,
        inputs: (type.inPorts ?? []).map(portView),
        outputs: (type.outPorts ?? []).map(portView),
    };
}
