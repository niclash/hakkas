// Fixed values on inputs (initials, 02 §3.4) as the inspector edits them (06 §4.2). Kept free
// of Vue and Rete so that it can be unit tested.

// What feeds an input: a connection, a fixed value (as the event carries it, a string), or
// nothing (its port's default).
export type InputSource =
    | { kind: "connected", from: string }   // "<node>.<port>"
    | { kind: "fixed", value: string }
    | { kind: "bound", channel: string, port: string }   // a sensor, "ain[2]", through process inport `port` (06 §4.14)
    | { kind: "default" };

// The editor that fits a port's type. A type other than "bool" or "string" is parsed by the
// engine like "number" (02 §3.4, initial value encoding).
export type ValueEditor = "number" | "bool" | "string";

export function valueEditor(portType: string): ValueEditor {
    return portType === "bool" || portType === "string" ? portType : "number";
}

// What an input holds when nothing feeds it: its type's resting value (decided with the user
// 2026-10-14: no per-port defaults; an absent signal rests at 0, false or "", 03 §1).
export function restingValue(portType: string): string {
    const editor = valueEditor(portType);
    return editor === "bool" ? "false" : editor === "string" ? "" : "0";
}

// The value to send for what was typed into a number or text field (a bool port has a
// switch): `clear` to remove the fixed value (an empty number field), or an error to show
// instead of sending. A number goes as a JSON number: the engine would read text that is not
// wholly a number as 0 (02 §3.4).
export function parseTyped(editor: "number" | "string", text: string):
    { value: number | string } | { clear: true } | { error: string } {
    if (editor === "string")
        return {value: text};
    const trimmed = text.trim();
    if (trimmed === "")
        return {clear: true};
    const n = Number(trimmed);
    if (!Number.isFinite(n))
        return {error: "Enter a number"};
    return {value: n};
}

// Whether an event's value is the same as the one typed: unchanged values are not sent.
export function sameValue(editor: ValueEditor, current: string, typed: number | string | boolean): boolean {
    if (editor === "number")
        return current.trim() !== "" && Number(current) === typed;
    return current === String(typed);
}

// The refusals a fixed-value command on node.port can cause (02 §3.4.1): while it is
// pending, they are shown at the input instead of with the other errors.
export function initialRefusals(node: string, port: string, command: "addinitial" | "removeinitial"): string[] {
    const common = ["Unknown node: " + node, "Unknown port: " + node + "." + port, "Not permitted: graph:" + command];
    return command === "addinitial"
        ? ["Input already connected: " + node + "." + port, ...common]
        : ["No such connection", ...common];
}
