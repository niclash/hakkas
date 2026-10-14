// The editor's context menus (06 §4.4): which items a right-click offers, by what was clicked.
// Kept free of Vue and Rete so that it can be unit tested; ReteController carries out the
// actions, each as commands to the engine.

import {InputSource} from "@/components/initials";

// `watched`: whether the input (a block: any of its inputs) has its live value watched (06 §4.7).
export type MenuTarget =
    | { kind: "canvas", watchingAll?: boolean }
    | { kind: "block", id: string, selected: number, watched?: boolean }   // `selected`: blocks selected with it
    | { kind: "input", id: string, port: string, source: InputSource, watched?: boolean }
    | { kind: "value", id: string, port: string, watched?: boolean }       // a value node or its line (06 §4.3)
    | { kind: "connection", watched?: boolean }
    | { kind: "output", id: string, port: string }                          // a block's output row
    | { kind: "port", direction: "in" | "out", name: string };              // a process port (06 §4.8)

export type MenuAction =
    | "add-block" | "zoom-fit" | "arrange" | "select-all"
    | "rename" | "edit-inputs" | "delete-block" | "delete-selection"
    | "set-value" | "change-value" | "clear-value" | "disconnect"
    | "delete-connection"
    | "watch" | "unwatch" | "watch-inputs" | "unwatch-inputs" | "watch-all" | "unwatch-all"
    | "expose-input" | "expose-output" | "rename-port" | "remove-port";

export interface MenuItem {
    action: MenuAction;
    label: string;
    shortcut?: string;
}

const watchItem = (watched: boolean | undefined): MenuItem =>
    watched ? {action: "unwatch", label: "Stop watching"} : {action: "watch", label: "Watch value"};

export function menuFor(target: MenuTarget): MenuItem[] {
    switch (target.kind) {
        case "canvas":
            return [
                {action: "add-block", label: "Add block…"},
                {action: "select-all", label: "Select all", shortcut: "Ctrl+A"},
                {action: "zoom-fit", label: "Zoom to fit"},
                {action: "arrange", label: "Arrange blocks"},
                target.watchingAll
                    ? {action: "unwatch-all", label: "Stop watching all connections"}
                    : {action: "watch-all", label: "Watch all connections"},
            ];
        case "block":
            return [
                {action: "rename", label: "Rename", shortcut: "F2"},
                {action: "edit-inputs", label: "Edit inputs"},
                target.watched
                    ? {action: "unwatch-inputs", label: "Stop watching inputs"}
                    : {action: "watch-inputs", label: "Watch inputs"},
                target.selected > 1
                    ? {action: "delete-selection", label: `Delete ${target.selected} blocks`, shortcut: "Del"}
                    : {action: "delete-block", label: "Delete", shortcut: "Del"},
            ];
        case "input": {
            const expose: MenuItem = {action: "expose-input", label: "Expose as process input…"};
            if (target.source.kind === "connected")
                return [{action: "disconnect", label: "Disconnect"}, watchItem(target.watched), expose];
            if (target.source.kind === "fixed")
                return [
                    {action: "change-value", label: "Change fixed value…"},
                    {action: "clear-value", label: "Use the default"},
                    watchItem(target.watched),
                    expose,
                ];
            return [{action: "set-value", label: "Set fixed value…"}, watchItem(target.watched), expose];
        }
        case "output":
            return [{action: "expose-output", label: "Expose as process output…"}];
        case "port":
            return [
                {action: "rename-port", label: "Rename process port…"},
                {action: "remove-port", label: "Remove process port"},
            ];
        case "value":
            return [
                {action: "change-value", label: "Change value…"},
                {action: "clear-value", label: "Remove value"},
                watchItem(target.watched),
            ];
        case "connection":
            return [{action: "delete-connection", label: "Delete connection"}, watchItem(target.watched)];
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
