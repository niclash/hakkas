// Where process ports are drawn (06 §4.8): inputs left of everything in the process, outputs
// right of it, each level with its block port, moved down where two would overlap. Kept free of
// Rete so that it can be unit tested.

export type Box = { x: number, y: number, width: number, height: number };

export interface PortMarker {
    id: string;                 // the marker's own id
    direction: "in" | "out";
    socketY: number;            // the block port's socket, in the editor's coordinates
    width: number;
    height: number;
}

// The space between the process and its port markers, and between two markers.
export const PORT_GAP = 60;
export const PORT_SPACING = 6;

export function layoutPorts(content: Box[], markers: PortMarker[]): Map<string, { x: number, y: number }> {
    const result = new Map<string, { x: number, y: number }>();
    if (content.length === 0)
        return result;
    const left = Math.min(...content.map(b => b.x));
    const right = Math.max(...content.map(b => b.x + b.width));
    for (const direction of ["in", "out"] as const) {
        const column = markers.filter(m => m.direction === direction)
            .sort((a, b) => a.socketY - b.socketY || a.id.localeCompare(b.id));
        let free = -Infinity;
        for (const m of column) {
            const y = Math.max(m.socketY - m.height / 2, free);
            const x = direction === "in" ? left - PORT_GAP - m.width : right + PORT_GAP;
            result.set(m.id, {x, y});
            free = y + m.height + PORT_SPACING;
        }
    }
    return result;
}
