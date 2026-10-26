// What the session may do (06 §4.11), from the capabilities of its login (02 §2.4). The UI only
// stops offering what the engine would refuse; the engine's refusals stay the guard. Kept free
// of Vue so that it can be unit tested.

import {MenuAction} from "@/components/floweditor/menus";

export interface Permissions {
    edit: boolean;      // protocol:graph — change a process or a type: blocks, connections, values, ports
    control: boolean;   // network:control — start and stop processes
    persist: boolean;   // network:persist — save a process or a type
    manage: boolean;    // protocol:runtime — create and delete processes (and types)
    watch: boolean;     // network:data — live values
}

export function permissions(capabilities: readonly string[]): Permissions {
    const has = (c: string) => capabilities.includes(c);
    return {
        edit: has("protocol:graph"),
        control: has("network:control"),
        persist: has("network:persist"),
        manage: has("protocol:runtime"),
        watch: has("network:data"),
    };
}

// What a role is called in the top bar: the most it may do.
export function roleLabel(p: Permissions): string {
    if (p.edit)
        return "";
    return p.control ? "Operator — starts and stops, no editing" : "Viewer — read only";
}

const needs: Partial<Record<MenuAction, (p: Permissions) => boolean>> = {
    "add-block": p => p.edit, "paste": p => p.edit, "arrange": p => p.edit,
    "rename": p => p.edit, "edit-inputs": p => p.edit, "delete-block": p => p.edit,
    "delete-selection": p => p.edit, "duplicate": p => p.edit, "cut": p => p.edit,
    "create-composite": p => p.edit && p.persist,
    "set-value": p => p.edit, "change-value": p => p.edit, "clear-value": p => p.edit,
    "disconnect": p => p.edit, "delete-connection": p => p.edit,
    "expose-input": p => p.edit, "expose-output": p => p.edit,
    "rename-port": p => p.edit, "remove-port": p => p.edit,
    "bind": p => p.edit, "unbind": p => p.edit,
    "watch": p => p.watch, "unwatch": p => p.watch, "watch-inputs": p => p.watch,
    "unwatch-inputs": p => p.watch, "watch-all": p => p.watch, "unwatch-all": p => p.watch,
};

// Whether the menu item may be offered (select all, zoom, copy: always).
export function allowed(action: MenuAction, p: Permissions): boolean {
    return needs[action]?.(p) ?? true;
}
