// The scene (control protocol §3.4, spec §8.3): the board, a part per I/O slot, and a row per
// channel and signal, built from the emulated system. Pictures are served under `assets/`.
import type {Catalog} from './catalog.js';
import type {Json, JsonObject} from './check.js';
import {isMeasured, type PictureItem} from './descriptors.js';
import type {System} from './system.js';

export const MODULE_ASSETS = 'assets/modules';
export const CARRIER_ASSETS = 'assets/carriers';

export interface Scene {
    board: {asset: string} | null;
    parts: JsonObject[];
    rows: JsonObject[];
}

export const EMPTY_SCENE: Scene = {board: null, parts: [], rows: []};

export function scene(system: System, catalog: Catalog): Scene {
    const carrier = system.carrier;
    const board = carrier.picture !== undefined && catalog.carriers.get(carrier.type)?.def === carrier
        ? {asset: `${CARRIER_ASSETS}/${carrier.type}/${carrier.picture}`} : null;
    const parts: JsonObject[] = [];
    const rows: JsonObject[] = [];
    for (const slot of system.slots) {
        const m = slot.module;
        const part: JsonObject = {id: slot.part, place: `slot-${slot.index}`, type: m?.type ?? null,
            name: m === undefined ? null : m.name ?? m.type};
        if (slot.builtin)
            part.builtin = true;
        if (m?.faceplate !== undefined) {
            part.asset = `${MODULE_ASSETS}/${m.type}/${m.faceplate}`;
            part.bindings = m.bindings.map(b => complete(b, slot.part));
            part.controls = m.controls.map(b => complete(b, slot.part));
        }
        parts.push(part);
        for (const s of slot.signals)
            rows.push(optional({key: s.key, part: slot.part, label: s.label, kind: 'signal', type: 'pulse',
                description: s.def.description, sources: ['pulse']}));
        for (const ch of [...slot.inputs, ...slot.outputs]) {
            const d = ch.def;
            const row: Record<string, Json | undefined> = {key: ch.key, part: slot.part, label: ch.label,
                kind: ch.kind, type: d.type, unit: d.unit, min: d.min, max: d.max,
                resolution: d.direction === 'input' ? d.resolution : undefined, description: d.description};
            if (d.direction === 'output')
                row.indicator = d.indicator;
            else if (!isMeasured(d)) {
                row.control = d.control;
                row.sources = d.sources;
            }
            rows.push(optional(row));
        }
    }
    return {board, parts, rows};
}

// A picture item's key relative to its module, made complete: `dq.1` in slot 3 is `slot3.dq.1`.
function complete(item: PictureItem, part: string): JsonObject {
    const out: JsonObject = {...item};
    for (const k of ['key', 'text', 'visible'])
        if (typeof out[k] === 'string')
            out[k] = `${part}.${out[k] as string}`;
    return out;
}

function optional(o: Record<string, Json | undefined>): JsonObject {
    return Object.fromEntries(Object.entries(o).filter(([, v]) => v !== undefined)) as JsonObject;
}
