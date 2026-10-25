// The settings form (01-control-protocol.md §3.3) as the UI lays it out, and the counters
// (§3.6) as it lists them. Kept free of Vue so that it can be unit tested.
import type {Field, Json, Row, Scene, Stats} from './protocol/types';

export interface FieldGroup {
    title: string;          // "" for the fields without a group
    fields: Field[];
}

// The fields without a group first, then each group in the order its first field comes.
export function groupFields(fields: Field[]): FieldGroup[] {
    const groups: FieldGroup[] = [{title: '', fields: []}];
    for (const f of fields) {
        const title = f.group ?? '';
        let g = groups.find(x => x.title === title);
        if (g === undefined) {
            g = {title, fields: []};
            groups.push(g);
        }
        g.fields.push(f);
    }
    return groups.filter(g => g.fields.length > 0);
}

// A list field's value as text, one entry per comma, and back (empty entries dropped).
export function listText(value: Json | undefined): string {
    return Array.isArray(value) ? value.join(', ') : '';
}

export function parseList(text: string): string[] {
    return text.split(',').map(s => s.trim()).filter(s => s !== '');
}

// An enum option's value for a <select>, and back: options may be numbers (a bitrate).
export function optionKey(value: Json): string {
    return JSON.stringify(value);
}

export interface PartCounters {
    id: string;
    name: string;
    queue: number;
    queueMax: number;
    dropped: number;
}

export interface ChannelCounters {
    key: string;
    label: string;
    part: string;
    output: boolean;
    received?: number;
    queued?: number;
    dropped?: number;
    sent?: number;
}

// The counters in the scene's order: a part per filled slot that has them, its channels after
// it (outputs' writes, inputs' values sent).
export function countersOf(stats: Stats, scene: Scene): {parts: PartCounters[], channels: ChannelCounters[]} {
    const parts: PartCounters[] = scene.parts
        .filter(p => stats.parts[p.id] !== undefined)
        .map(p => ({id: p.id, name: p.name ?? p.type ?? p.id, ...stats.parts[p.id]!}));
    const channels: ChannelCounters[] = scene.rows
        .filter((r: Row) => r.kind !== 'signal' && stats.channels[r.key] !== undefined)
        .map((r: Row) => ({key: r.key, label: r.label, part: r.part, output: r.kind === 'output', ...stats.channels[r.key]}));
    return {parts, channels};
}
