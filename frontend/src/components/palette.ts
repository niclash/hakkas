// The block palette (06 §4.5): searching it, and what a type's tooltip says. Kept free of Vue
// so that it can be unit tested.

import {ComponentEvent} from "@/components/protocols/component";
import {searchTypes} from "@/components/floweditor/menus";

// The data type a palette entry carries when it is dragged onto the editor.
export const BLOCK_TYPE_MIME = "application/x-hakkas-block-type";

export interface PaletteEntry {
    name: string;      // the type's full name, e.g. "math/Add2"
    short: string;     // its name after the family, e.g. "Add2"
    tooltip: string;
    subgraph: boolean; // a composite type: it can be opened (06 §4.9)
}

// The types grouped by family (the prefix before "/"), in the order they arrived, keeping only
// those that match the search (as the context menu's Add block… does).
export function paletteGroups(types: ComponentEvent[], search: string): Record<string, PaletteEntry[]> {
    const shown = new Set(searchTypes(types.map(t => t.name), search));
    const groups: Record<string, PaletteEntry[]> = {};
    for (const type of types) {
        if (!shown.has(type.name))
            continue;
        const slash = type.name.indexOf("/");
        const family = slash < 0 ? "" : type.name.substring(0, slash);
        (groups[family] ??= []).push({
            name: type.name, short: type.name.substring(slash + 1), tooltip: typeTooltip(type),
            subgraph: type.subgraph === true,
        });
    }
    return groups;
}

// The type's description and its ports with their types.
export function typeTooltip(type: ComponentEvent): string {
    const ports = (list: ComponentEvent["inPorts"] | undefined) =>
        (list ?? []).map(p => p.type ? `${p.id} (${p.type})` : p.id).join(", ");
    const lines = [type.name];
    if (type.description)
        lines.push(type.description);
    const inputs = ports(type.inPorts);
    const outputs = ports(type.outPorts);
    if (inputs)
        lines.push("Inputs: " + inputs);
    if (outputs)
        lines.push("Outputs: " + outputs);
    return lines.join("\n");
}

// The socket class of a port type: the three types the engine has, or "other".
export function socketKind(portType: string): "number" | "bool" | "string" | "other" {
    return portType === "number" || portType === "bool" || portType === "string" ? portType : "other";
}
