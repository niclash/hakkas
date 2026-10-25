// Logging (spec §3.1): one line per event, `<level> <message>`, on standard error by default.
// The levels and their order are hakkas' (05 §4).

export const LEVELS = ['fine', 'info', 'warn', 'error'] as const;
export type Level = typeof LEVELS[number];

export function isLevel(s: string): s is Level {
    return (LEVELS as readonly string[]).includes(s);
}

export interface Logger {
    fine(message: string): void;
    info(message: string): void;
    warn(message: string): void;
    error(message: string): void;
}

// A message is written only when its level is at or above `threshold`. A message's line breaks
// are written as spaces, so one event is always one line.
export function createLogger(threshold: Level, write: (line: string) => void = stderrLine): Logger {
    const min = LEVELS.indexOf(threshold);
    const at = (level: Level) => (message: string) => {
        if (LEVELS.indexOf(level) >= min)
            write(`${level} ${message.replace(/[\r\n]+/g, ' ')}`);
    };
    return {fine: at('fine'), info: at('info'), warn: at('warn'), error: at('error')};
}

function stderrLine(line: string): void {
    process.stderr.write(line + '\n');
}
