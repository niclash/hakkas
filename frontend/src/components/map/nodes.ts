// The process map's Rete nodes (06 §4.17): one per process, its inports as inputs and its
// outports as outputs, all on one socket (a link joins any outport to any inport).
import {ClassicPreset, GetSchemes} from "rete";

export const linkSocket = new ClassicPreset.Socket("link");

export class ProcessNode extends ClassicPreset.Node {
    width = 340;
    height: number;

    constructor(readonly graph: string, readonly inports: string[], readonly outports: string[]) {
        super(graph);
        this.id = "process:" + graph;
        for (const name of inports)
            this.addInput(name, new ClassicPreset.Input(linkSocket, name));
        for (const name of outports)
            this.addOutput(name, new ClassicPreset.Output(linkSocket, name));
        // The title, then one row per port on the longer side (MapNodeView's sizes), until it is
        // measured as drawn.
        const rows = Math.max(inports.length, outports.length);
        this.height = rows === 0 ? 48 : 52 + 34 * rows;
        if (rows === 0)
            this.width = 240;
    }
}

// Over the base node type: Rete's classic presets need a connection of any classic node.
export class LinkConnection extends ClassicPreset.Connection<ClassicPreset.Node, ClassicPreset.Node> {
}

export type MapSchemes = GetSchemes<ProcessNode, LinkConnection>;
