// The scene (control protocol §3.4, spec §8.3): the board, a part per I/O slot, and a row per
// channel and signal, built from the emulated system. Pictures are served under `assets/`.
//
// The board is the carrier's SVG picture with places by element id (§7.4), or else the render
// copies (spec §8.4, hakkas plan task 77): the carrier's copy as the board, each part's in its
// slot's place (`rect`), the communication and MCU modules' as decoration (`extras`), and each
// row with a position at its point on the board (`at`) — the same pictures and places as hakkas'
// device window.
import type {Catalog} from './catalog.js';
import type {Json, JsonObject} from './check.js';
import {isMeasured, type McuType, type PictureItem, type Rect} from './descriptors.js';
import type {Slot, System} from './system.js';

export const MODULE_ASSETS = 'assets/modules';
export const CARRIER_ASSETS = 'assets/carriers';
export const COMM_ASSETS = 'assets/comm';
export const MCU_ASSETS = 'assets/mcu';

export interface Scene {
    board: JsonObject | null;
    parts: JsonObject[];
    rows: JsonObject[];
}

export const EMPTY_SCENE: Scene = {board: null, parts: [], rows: []};

// `mcu`: the MCU module in the carrier's MCU slot, if it takes it.
export function scene(system: System, catalog: Catalog, mcu?: McuType): Scene {
    const carrier = system.carrier;
    const known = catalog.carriers.get(carrier.type)?.def === carrier;
    let board: JsonObject | null = null;
    // The render copies' board: the carrier's copy and its places.
    const places = known && carrier.picture === undefined && carrier.render?.copy !== undefined
        ? carrier.places : undefined;
    if (known && carrier.picture !== undefined)
        board = {asset: `${CARRIER_ASSETS}/${carrier.type}/${carrier.picture}`};
    else if (places !== undefined)
        board = {asset: `${CARRIER_ASSETS}/${carrier.type}/${carrier.render!.copy!}`,
            extras: extras(system, places, mcu)};
    const parts: JsonObject[] = [];
    const rows: JsonObject[] = [];
    for (const slot of system.slots) {
        const m = slot.module;
        const part: JsonObject = {id: slot.part, place: `slot-${slot.index}`, type: m?.type ?? null,
            name: m === undefined ? null : m.name ?? m.type};
        if (slot.builtin)
            part.builtin = true;
        const rect = places?.slots.find(r => r.slot === slot.index);
        if (rect !== undefined && !slot.builtin)
            part.rect = rectOf(rect);
        if (m?.faceplate !== undefined) {
            part.asset = `${MODULE_ASSETS}/${m.type}/${m.faceplate}`;
            part.bindings = m.bindings.map(b => complete(b, slot.part));
            part.controls = m.controls.map(b => complete(b, slot.part));
        } else if (places !== undefined && m?.render?.copy !== undefined) {
            part.asset = `${MODULE_ASSETS}/${m.type}/${m.render.copy}`;
        }
        parts.push(part);
        const at = places !== undefined ? pointsOf(slot, slot.builtin ? undefined : rect)
            : new Map<string, JsonObject>();
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
            const point = at.get(ch.key);
            if (point !== undefined)
                row.at = point;
            rows.push(optional(row));
        }
    }
    return {board, parts, rows};
}

const rectOf = (r: Rect): JsonObject => ({x: r.x, y: r.y, w: r.w, h: r.h});

// The points of a slot's channels on the board, by key: its module's positions within its place,
// a built-in module's on the board itself (04 §1). Without a place, none.
function pointsOf(slot: Slot, rect: Rect | undefined): Map<string, JsonObject> {
    const out = new Map<string, JsonObject>();
    if (slot.module === undefined || (!slot.builtin && rect === undefined))
        return out;
    const r = rect ?? {x: 0, y: 0, w: 1, h: 1};
    for (const p of slot.module.positions)
        out.set(`${slot.part}.${p.channel}${p.index === undefined ? '' : `.${p.index}`}`,
            {x: r.x + p.x * r.w, y: r.y + p.y * r.h});
    return out;
}

// The communication modules' and the MCU module's copies in their places: decoration.
function extras(system: System, places: NonNullable<System['carrier']['places']>, mcu?: McuType): JsonObject[] {
    const out: JsonObject[] = [];
    for (const s of system.comm) {
        const rect = places.comm.find(r => r.slot === s.slot);
        if (rect !== undefined)
            out.push({name: s.def.name ?? s.def.type, rect: rectOf(rect),
                ...(s.def.render?.copy !== undefined && {asset: `${COMM_ASSETS}/${s.def.type}/${s.def.render.copy}`})});
    }
    if (mcu !== undefined && places.mcu !== undefined)
        out.push({name: mcu.name ?? mcu.type, rect: rectOf(places.mcu),
            ...(mcu.render?.copy !== undefined && {asset: `${MCU_ASSETS}/${mcu.type}/${mcu.render.copy}`})});
    return out;
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
