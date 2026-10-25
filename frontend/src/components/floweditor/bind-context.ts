// What the binding checks (bindings.ts) need of the open process, read from the editor.

import {PrimitiveNode} from "@/components/model";
import type {Binding, EnvDescription} from "@/components/protocols/environment";
import {CheckContext} from "@/components/bindings";
import {editor} from "./viewmodel/default";
import {processPortNodes} from "./ports";
import {inputSources} from "./values";

// `bindings`: what stays bound while the entries are made (other processes' bindings, and the
// open process's unless they are replaced).
export function checkContext(description: EnvDescription, graph: string, bindings: Binding[]): CheckContext {
    const inports: Record<string, string> = {}, outports: Record<string, string> = {};
    for (const p of processPortNodes(editor)) {
        const block = editor.getNode(p.block);
        if (block instanceof PrimitiveNode)
            (p.direction === "in" ? inports : outports)[p.name] = block.label + "." + p.port;
    }
    const sourced = new Set<string>();
    for (const node of editor.getNodes()) {
        if (!(node instanceof PrimitiveNode))
            continue;
        for (const [port, source] of Object.entries(inputSources(editor, node)))
            if (source.kind === "connected" || source.kind === "fixed")
                sourced.add(node.label + "." + port);
    }
    return {description, inports, outports, sourced, bindings, graph};
}
