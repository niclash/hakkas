// The control protocol's messages (emulators/specifications/01-control-protocol.md §3), as the
// UI reads them.

export type Json = null | boolean | number | string | Json[] | {[key: string]: Json};
export type JsonObject = {[key: string]: Json};

export interface Hello {
    protocol: number;
    emulator: string;
    version: string;
    features: string[];
}

export interface Library {
    environments: {name: string}[];
    running: string | null;
    unsaved: boolean;
}

export interface Option {
    value: Json;
    label: string;
}

export interface Field {
    id: string;
    label: string;
    type: 'enum' | 'number' | 'integer' | 'text' | 'bool' | 'list';
    group?: string;
    description?: string;
    readonly?: boolean;
    restart?: boolean;
    options?: Option[];
    min?: number;
    max?: number;
    unit?: string;
}

export interface Settings {
    fields: Field[];
    values: JsonObject;
}

// A rectangle or a point on the board's picture, in fractions of its width and height.
export interface Rect {
    x: number;
    y: number;
    w: number;
    h: number;
}

export interface Point {
    x: number;
    y: number;
}

export interface Part {
    id: string;
    place: string;
    rect?: Rect;                // its place on a board drawn from pictures (§3.4)
    type: string | null;
    name: string | null;
    builtin?: boolean;
    asset?: string;
    bindings?: JsonObject[];
    controls?: JsonObject[];
}

export type RowKind = 'input' | 'signal' | 'measured' | 'output';
export type Control = 'slider' | 'number' | 'toggle' | 'button';
export type Indicator = 'lamp' | 'number' | 'bar' | 'meter';
export type SourceKind = 'constant' | 'sine' | 'square' | 'ramp' | 'counter' | 'sequence' | 'pulse';

export interface Row {
    key: string;
    part: string;
    label: string;
    kind: RowKind;
    type: 'number' | 'bool' | 'pulse';
    unit?: string;
    min?: number;
    max?: number;
    resolution?: number;
    description?: string;
    control?: Control;
    indicator?: Indicator;
    sources?: SourceKind[];
    at?: Point;                 // where it is on the board's picture (§3.4)
}

// A picture on the board that is not a part (a communication or MCU module).
export interface Extra {
    name: string;
    rect: Rect;
    asset?: string;
}

export interface Scene {
    board: {asset: string, extras?: Extra[]} | null;
    parts: Part[];
    rows: Row[];
}

export interface PartStats {
    queue: number;
    queueMax: number;
    dropped: number;
}

export interface ChannelStats {
    received?: number;
    queued?: number;
    dropped?: number;
    sent?: number;
}

export interface Stats {
    parts: Record<string, PartStats>;
    channels: Record<string, ChannelStats>;
}

export type LinkState = 'waiting' | 'connected' | 'restart';

export interface Notice {
    level: 'info' | 'warning' | 'error';
    message: string;
}
