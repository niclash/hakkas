// The editor's context menus (06 §4.4): which items a right-click offers, by what was clicked.
// Kept free of Vue and Rete so that it can be unit tested; ReteController carries out the
// actions, each as commands to the engine.

import {InputSource} from "@/components/initials";

export type MenuTarget =
    | { kind: "canvas" }
    | { kind: "block", id: string, selected: number }   // `selected`: blocks selected with it, 0 if not
    | { kind: "input", id: string, port: string, source: InputSource }
    | { kind: "value", id: string, port: string }          // a value node or its line (06 §4.3)
    | { kind: "connection" };

export type MenuAction =
    | "add-block" | "zoom-fit" | "arrange" | "select-all"
    | "rename" | "edit-inputs" | "delete-block" | "delete-selection"
    | "set-value" | "change-value" | "clear-value" | "disconnect"
    | "delete-connection";

export interface MenuItem {
    action: MenuAction;
    label: string;
    shortcut?: string;
}

export function menuFor(target: MenuTarget): MenuItem[] {
    switch (target.kind) {
        case "canvas":
            return [
                {action: "add-block", label: "Add block…"},
                {action: "select-all", label: "Select all", shortcut: "Ctrl+A"},
                {action: "zoom-fit", label: "Zoom to fit"},
                {action: "arrange", label: "Arrange blocks"},
            ];
        case "block":
            return [
                {action: "rename", label: "Rename", shortcut: "F2"},
                {action: "edit-inputs", label: "Edit inputs"},
                target.selected > 1
                    ? {action: "delete-selection", label: `Delete ${target.selected} blocks`, shortcut: "Del"}
                    : {action: "delete-block", label: "Delete", shortcut: "Del"},
            ];
        case "input":
            if (target.source.kind === "connected")
                return [{action: "disconnect", label: "Disconnect"}];
            if (target.source.kind === "fixed")
                return [
                    {action: "change-value", label: "Change fixed value…"},
                    {action: "clear-value", label: "Use the default"},
                ];
            return [{action: "set-value", label: "Set fixed value…"}];
        case "value":
            return [
                {action: "change-value", label: "Change value…"},
                {action: "clear-value", label: "Remove value"},
            ];
        case "connection":
            return [{action: "delete-connection", label: "Delete connection"}];
    }
}

// The type names that match a search in the block list, the closest first: a name that starts
// with the text (after its family prefix), then one that contains it; case does not matter.
export function searchTypes(names: string[], text: string): string[] {
    const q = text.trim().toLowerCase();
    if (q === "")
        return [...names];
    const short = (name: string) => name.substring(name.lastIndexOf("/") + 1).toLowerCase();
    const starts = names.filter(n => short(n).startsWith(q));
    const contains = names.filter(n => !starts.includes(n) && n.toLowerCase().includes(q));
    return [...starts, ...contains];
}

// The next item for an arrow key in a menu of `count` items, wrapping at the ends.
export function nextIndex(current: number, count: number, key: "ArrowDown" | "ArrowUp" | "Home" | "End"): number {
    if (count === 0)
        return -1;
    switch (key) {
        case "ArrowDown": return (current + 1) % count;
        case "ArrowUp": return current <= 0 ? count - 1 : current - 1;
        case "Home": return 0;
        case "End": return count - 1;
    }
}
