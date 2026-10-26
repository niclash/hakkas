// The device window's model (06 §4.15, plan task 78): what is drawn where on the carrier's
// picture, and every channel's live value (environment:watch, 02 §3.7), while the window is open.

import {reactive} from "vue";
import type {EnvDescription, EnvModule, EnvPicture, EnvPort, EnvRect, EnvValue} from "@/components/protocols/environment";
import {Connection} from "@/components/protocols/websocket";

export const device = reactive({
    open: false,
    popped: false,                      // in a window of its own
    // The live values by channel (valueKey); a channel without one has none yet: a sensor no
    // value came for, an actuator never written (not driven).
    values: {} as Record<string, number | boolean | null>,
});

export function valueKey(env: string, index?: number): string {
    return `${env}[${index ?? 0}]`;
}

let watching: Connection | undefined;

// Feeds the values from a connection's environment:values; called once per connection.
export function attachDevice(conn: Connection) {
    device.values = {};
    watching = undefined;
    conn.proto.environment.addListener("onValues", (p: { values?: EnvValue[] }) => {
        for (const v of p.values ?? [])
            if (typeof v?.env === "string")
                device.values[valueKey(v.env, v.index)] = v.value;
    });
}

// Watching while the window is open (02 §3.7): every channel with a value, then each change.
export function watchDevice(conn: Connection | undefined, on: boolean) {
    if (on && conn !== undefined && watching !== conn) {
        device.values = {};
        conn.proto.environment.request_watch();
        watching = conn;
    } else if (!on && watching !== undefined) {
        watching.proto.environment.request_unwatch();
        watching = undefined;
    }
}

// ---- Layout -----------------------------------------------------------------------------

// A point on the carrier's picture as shown, in fractions of it.
export interface Badge {
    env: string;
    index?: number;
    port: EnvPort;
    sensor: boolean;
    x: number;
    y: number;
    stack: number;                      // how many badges came before it at the same point
}

export interface Part {
    key: string;
    kind: "module" | "comm" | "mcu";
    rect: EnvRect;                      // on the carrier's picture, in fractions of it
    picture?: EnvPicture;
    label: string;                      // its name, else its type
    title: string;                      // the tooltip
    documentation?: string;
}

export interface Layout {
    parts: Part[];
    badges: Badge[];
    // The channels with no point on the picture, by module (slot order), then those of no module.
    others: { label: string, channels: { env: string, index?: number, port: EnvPort, sensor: boolean }[] }[];
}

const fraction = (v: unknown): v is number => typeof v === "number" && v >= 0 && v <= 1;
const rectOk = (r: EnvRect | undefined): r is EnvRect =>
    r !== undefined && [r.x, r.y, r.w, r.h].every(fraction) && r.w > 0 && r.h > 0;

function portOf(d: EnvDescription, id: string): { port: EnvPort, sensor: boolean } | undefined {
    const out = d.outPorts.find(p => p.id === id);
    if (out !== undefined)
        return {port: out, sensor: true};
    const inp = d.inPorts.find(p => p.id === id);
    return inp !== undefined ? {port: inp, sensor: false} : undefined;
}

// The channels of a port: 1..count, or one without an index.
function channelsOf(port: EnvPort): (number | undefined)[] {
    return port.addressable ? Array.from({length: port.count ?? 0}, (_, i) => i + 1) : [undefined];
}

const tooltip = (m: { name?: string, type?: string, vendor?: string, revision?: string, serial?: string }) =>
    [m.name ?? m.type, m.name !== undefined ? m.type : undefined, m.vendor, m.revision && "rev. " + m.revision,
        m.serial && "serial " + m.serial].filter(s => s).join(" · ");

// What the description puts where (04 §1 "Pictures"): every module in a slot with a place, the
// communication modules and the MCU module, and each channel with a usable position. A
// position naming no channel, or outside the picture, is skipped (hakkas does not check them).
export function layout(d: EnvDescription): Layout {
    const places = d.carrier?.places;
    const parts: Part[] = [];
    const badges: Badge[] = [];
    const placed = new Set<string>();
    const slotRect = (slot: number | undefined) => places?.slots?.find(s => s.slot === slot);

    const place = (m: EnvModule, rect: EnvRect | undefined) => {
        for (const p of m.positions ?? []) {
            const at = portOf(d, p.port);
            if (at === undefined || !fraction(p.x) || !fraction(p.y) || !channelsOf(at.port).includes(p.index ?? undefined))
                continue;
            const key = valueKey(p.port, p.index);
            if (placed.has(key))
                continue;
            placed.add(key);
            // A built-in module's positions are on the carrier's picture; another's on its own,
            // drawn into its slot.
            const r = rect ?? {x: 0, y: 0, w: 1, h: 1};
            const x = r.x + p.x * r.w, y = r.y + p.y * r.h;
            const stack = badges.filter(b => b.x === x && b.y === y).length;
            badges.push({env: p.port, index: p.index, ...at, x, y, stack});
        }
    };

    d.modules.forEach((m, i) => {
        const rect = slotRect(m.slot);
        if (m.builtin) {
            place(m, undefined);
        } else if (rectOk(rect)) {
            parts.push({key: "m" + i, kind: "module", rect, picture: m.picture, label: m.name ?? m.type ?? "module",
                title: (m.slot !== undefined ? `Slot ${m.slot}: ` : "") + tooltip(m), documentation: m.documentation});
            place(m, rect);
        }
    });
    for (const c of d.carrier?.comm ?? []) {
        const rect = places?.comm?.find(s => s.slot === c.slot);
        if (rectOk(rect))
            parts.push({key: "c" + c.slot, kind: "comm", rect, picture: c.picture, label: c.name ?? c.type ?? "comm",
                title: `Comm ${c.slot}: ` + tooltip(c)});
    }
    const mcu = d.carrier?.mcu;
    if (mcu !== undefined && rectOk(places?.mcu))
        parts.push({key: "mcu", kind: "mcu", rect: places!.mcu!, picture: mcu.picture, label: mcu.name ?? mcu.type ?? "MCU",
            title: "MCU: " + tooltip(mcu), documentation: mcu.documentation});

    const others: Layout["others"] = [];
    const listed = new Set<string>();
    const unplaced = (ports: string[]) => ports.flatMap(id => {
        listed.add(id);
        const at = portOf(d, id);
        return at === undefined ? [] : channelsOf(at.port).filter(i => !placed.has(valueKey(id, i)))
            .map(index => ({env: id, index, ...at}));
    });
    for (const m of d.modules) {
        const channels = unplaced(m.ports);
        if (channels.length > 0)
            others.push({label: (m.slot !== undefined ? `Slot ${m.slot}: ` : "") + (m.name ?? m.type ?? "module"), channels});
    }
    const rest = unplaced([...d.outPorts, ...d.inPorts].map(p => p.id).filter(id => !listed.has(id)));
    if (rest.length > 0)
        others.push({label: d.modules.length > 0 ? "Other ports" : "Ports", channels: rest});
    return {parts, badges, others};
}

// A value as a badge on the picture shows it, where room is short: three significant digits
// (whole numbers from 1000 on), no unit; on/off as the badge's fill, the text only for numbers.
export function badgeText(v: number | boolean | null | undefined): string {
    if (typeof v !== "number")
        return valueText(v);
    return Math.abs(v) >= 1000 ? String(Math.round(v)) : String(Number(v.toPrecision(3)));
}

// A value as the window shows it: a number with up to three decimals and its unit, on/off.
export function valueText(v: number | boolean | null | undefined, unit?: string): string {
    if (v === undefined)
        return "–";
    if (v === null)
        return "NaN";
    if (typeof v === "boolean")
        return v ? "on" : "off";
    const text = Number.isInteger(v) ? String(v) : String(Number(v.toFixed(3)));
    return unit ? `${text} ${unit}` : text;
}
