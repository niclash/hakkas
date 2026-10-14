// The messages panel (06 §4.12): the engine's mirrored log lines (network:output and an error
// line's network:error, 05 §4) and the refusals shown in the error bar, newest last, the
// oldest dropped beyond a limit. Kept free of Vue so that it can be unit tested.

export type Level = "fine" | "info" | "warn" | "error";

export interface Message {
    seq: number;
    level: Level;
    text: string;
    source?: string;    // a log line's "<file>:<line>"
    refused?: boolean;  // a refusal of a command (02 §3.4.1), not a log line
}

// The panel keeps this many.
export const MESSAGES_KEPT = 200;

// A mirrored log line: "<level> <file>:<line>: <message>" (05 §4), else undefined.
export function parseLogLine(line: string): { level: Level, source: string, text: string } | undefined {
    const m = /^(fine|info|warn|error) (\S+:\d+): (.*)$/s.exec(line);
    return m === null ? undefined : {level: m[1] as Level, source: m[2], text: m[3]};
}

// An error as the error bar shows it: named after its process when that is not the one open.
export function errorText(message: string, process: string | undefined, open: boolean): string {
    return process !== undefined && process !== "" && !open ? process + ": " + message : message;
}

export class MessageLog {
    private seq = 0;
    readonly items: Message[] = [];

    constructor(private readonly kept = MESSAGES_KEPT) {
    }

    add(m: Omit<Message, "seq">): Message {
        const item = {...m, seq: ++this.seq};
        this.items.push(item);
        if (this.items.length > this.kept)
            this.items.splice(0, this.items.length - this.kept);
        return item;
    }

    clear() {
        this.items.splice(0);
    }
}

// The levels shown at or above the threshold chosen in the panel.
export function shown(items: readonly Message[], threshold: Level): Message[] {
    const order: Level[] = ["fine", "info", "warn", "error"];
    return items.filter(m => order.indexOf(m.level) >= order.indexOf(threshold));
}
