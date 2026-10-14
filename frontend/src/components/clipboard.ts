// Copy, paste and duplicate (06 §4.10): the selected blocks with the connections between them
// and their fixed values, as JSON (also on the system clipboard), and the commands that paste
// them into a process. Kept free of Vue and Rete so that it can be unit tested.

export const CLIP_FORMAT = "hakkas/blocks";

export interface ClipBlock {
    id: string;
    component: string;
    x: number;
    y: number;
    initials: Record<string, string>;
}

export interface ClipLink {
    src: { node: string, port: string };
    tgt: { node: string, port: string };
}

export interface Clip {
    format: typeof CLIP_FORMAT;
    version: 1;
    blocks: ClipBlock[];
    links: ClipLink[];      // only between copied blocks
}

export function makeClip(blocks: ClipBlock[], links: ClipLink[]): Clip {
    const inside = new Set(blocks.map(b => b.id));
    return {
        format: CLIP_FORMAT, version: 1,
        blocks: blocks.map(b => ({...b, initials: {...b.initials}})),
        links: links.filter(l => inside.has(l.src.node) && inside.has(l.tgt.node)),
    };
}

// The clip in text (pasted from the system clipboard), or undefined if it is none.
export function parseClip(text: string): Clip | undefined {
    let v: any;
    try {
        v = JSON.parse(text);
    } catch {
        return undefined;
    }
    const str = (s: unknown) => typeof s === "string";
    const end = (e: any) => e !== null && typeof e === "object" && str(e.node) && str(e.port);
    if (v === null || typeof v !== "object" || v.format !== CLIP_FORMAT || v.version !== 1
        || !Array.isArray(v.blocks) || !Array.isArray(v.links))
        return undefined;
    for (const b of v.blocks) {
        if (b === null || typeof b !== "object" || !str(b.id) || !str(b.component)
            || typeof b.x !== "number" || typeof b.y !== "number"
            || b.initials === null || typeof b.initials !== "object"
            || !Object.values(b.initials).every(str))
            return undefined;
    }
    if (!v.links.every((l: any) => l !== null && typeof l === "object" && end(l.src) && end(l.tgt)))
        return undefined;
    return makeClip(v.blocks, v.links);
}

// A free id for a pasted block: its own if free, else its base (without a trailing "_<n>")
// with the first free number, as the palette names blocks (Add2_1 → Add2_2).
export function freeId(id: string, taken: Set<string>): string {
    if (!taken.has(id))
        return id;
    const base = id.replace(/_\d+$/, "");
    for (let n = 1; ; n++) {
        if (!taken.has(base + "_" + n))
            return base + "_" + n;
    }
}

export interface Paste {
    ids: Map<string, string>;          // copied id → pasted id
    commands: { command: string, payload: Record<string, unknown> }[];
}

// The graph commands that paste clip into graph, whose blocks are taken: the blocks at their
// positions moved by (dx, dy), then the connections between them, then their fixed values.
export function pasteCommands(clip: Clip, graph: string, taken: Set<string>, dx: number, dy: number): Paste {
    const used = new Set(taken);
    const ids = new Map<string, string>();
    for (const b of clip.blocks) {
        const id = freeId(b.id, used);
        used.add(id);
        ids.set(b.id, id);
    }
    const commands: Paste["commands"] = [];
    for (const b of clip.blocks) {
        commands.push({command: "addnode", payload: {
            graph, id: ids.get(b.id), component: b.component,
            metadata: {x: Math.round(b.x + dx), y: Math.round(b.y + dy)},
        }});
    }
    for (const l of clip.links) {
        commands.push({command: "addedge", payload: {
            graph, src: {node: ids.get(l.src.node), port: l.src.port}, tgt: {node: ids.get(l.tgt.node), port: l.tgt.port},
        }});
    }
    for (const b of clip.blocks) {
        for (const [port, data] of Object.entries(b.initials))
            commands.push({command: "addinitial", payload: {graph, tgt: {node: ids.get(b.id), port}, src: {data}}});
    }
    return {ids, commands};
}

// The top-left corner of the clip's blocks (to paste them at a point).
export function clipOrigin(clip: Clip): { x: number, y: number } {
    return {x: Math.min(...clip.blocks.map(b => b.x)), y: Math.min(...clip.blocks.map(b => b.y))};
}
