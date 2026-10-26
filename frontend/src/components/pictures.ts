// The boards' pictures (04 §1 "Pictures", 06 §4.15; plan tasks 75, 77): where a picture comes
// from, and what it looks like as shown (cropped, then turned clockwise).
//
// The order: the browser's cache (IndexedDB, under the picture's url), the vendor's url (only
// https; cached when its host allows CORS, else shown as it is), then the device's own copy at
// /environment/pictures/<copy> (not cached: it is on the local network, and its name does not
// change with the picture).

import type {EnvPicture, EnvRect} from "@/components/protocols/environment";

// A picture ready to draw: its source, the file's size, and whether it is the device's copy
// (already cropped and turned, so the url's crop and rotation do not apply to it).
export interface Loaded {
    src: string;
    width: number;
    height: number;
    copy: boolean;
}

export interface PictureDeps {
    cacheGet(key: string): Promise<Blob | undefined>;
    cachePut(key: string, blob: Blob): Promise<void>;
    fetch(url: string): Promise<Blob | undefined>;          // undefined: refused (CORS) or failed
    size(src: string): Promise<{ width: number, height: number } | undefined>;   // undefined: no image
    objectUrl(blob: Blob): string;
}

export const COPY_PATH = "/environment/pictures/";

export function isHttps(url: string | undefined): url is string {
    return url !== undefined && /^https:\/\/\S+$/.test(url);
}

// Finds the picture's source in the order above; undefined when there is none.
export async function loadPicture(pic: EnvPicture, deps: PictureDeps): Promise<Loaded | undefined> {
    if (isHttps(pic.url)) {
        const blob = await deps.cacheGet(pic.url).catch(() => undefined) ?? await fetched(pic.url, deps);
        const src = blob !== undefined ? deps.objectUrl(blob) : pic.url;
        const size = await deps.size(src);
        if (size !== undefined)
            return {src, ...size, copy: false};
    }
    if (pic.copy !== undefined && pic.copy !== "" && !pic.copy.includes("/")) {
        const src = COPY_PATH + encodeURIComponent(pic.copy);
        const size = await deps.size(src);
        if (size !== undefined)
            return {src, ...size, copy: true};
    }
    return undefined;
}

async function fetched(url: string, deps: PictureDeps): Promise<Blob | undefined> {
    const blob = await deps.fetch(url).catch(() => undefined);
    if (blob !== undefined)
        await deps.cachePut(url, blob).catch(() => {});
    return blob;
}

// ---- Geometry ---------------------------------------------------------------------------

// A rotation as 0, 90, 180 or 270 (anything else counts as 0: not checked by hakkas).
export function rotationOf(pic: EnvPicture | undefined, loaded: Loaded): number {
    const r = pic?.rotation ?? 0;
    return loaded.copy || ![90, 180, 270].includes(r) ? 0 : r;
}

// The crop as fractions of the file: all of it for a copy, or a crop that is not inside it.
export function cropOf(pic: EnvPicture | undefined, loaded: Loaded): EnvRect {
    const c = pic?.crop;
    const inside = c !== undefined && [c.x, c.y, c.w, c.h].every(v => typeof v === "number" && v >= 0 && v <= 1)
        && c.w > 0 && c.h > 0 && c.x + c.w <= 1 + 1e-9 && c.y + c.h <= 1 + 1e-9;
    return loaded.copy || !inside ? {x: 0, y: 0, w: 1, h: 1} : c;
}

// The picture's size as shown, in the file's pixels.
export function shownSize(pic: EnvPicture | undefined, loaded: Loaded): { width: number, height: number } {
    const c = cropOf(pic, loaded);
    const w = c.w * loaded.width, h = c.h * loaded.height;
    return rotationOf(pic, loaded) % 180 === 0 ? {width: w, height: h} : {width: h, height: w};
}

// How to draw a picture into rect (SVG user units) so that it fills it as shown: the file is
// stretched to a unit square inside an inner box of `box` (the crop as its viewBox), which is
// turned about the rect's centre.
export function drawing(pic: EnvPicture | undefined, loaded: Loaded, rect: EnvRect):
    { transform: string, box: { x: number, y: number, w: number, h: number }, viewBox: string } {
    const r = rotationOf(pic, loaded);
    const c = cropOf(pic, loaded);
    const turned = r % 180 !== 0;
    const w = turned ? rect.h : rect.w, h = turned ? rect.w : rect.h;
    const cx = rect.x + rect.w / 2, cy = rect.y + rect.h / 2;
    return {
        transform: `translate(${cx} ${cy}) rotate(${r})`,
        box: {x: -w / 2, y: -h / 2, w, h},
        viewBox: `${c.x} ${c.y} ${c.w} ${c.h}`,
    };
}

// ---- The browser's side -----------------------------------------------------------------

const DB = "hakkas-pictures", STORE = "pictures";

function openDb(): Promise<IDBDatabase> {
    return new Promise((resolve, reject) => {
        const req = indexedDB.open(DB, 1);
        req.onupgradeneeded = () => req.result.createObjectStore(STORE);
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error);
    });
}

async function withStore<T>(mode: IDBTransactionMode, fn: (s: IDBObjectStore) => IDBRequest): Promise<T> {
    const db = await openDb();
    try {
        return await new Promise<T>((resolve, reject) => {
            const req = fn(db.transaction(STORE, mode).objectStore(STORE));
            req.onsuccess = () => resolve(req.result as T);
            req.onerror = () => reject(req.error);
        });
    } finally {
        db.close();
    }
}

export const browserDeps: PictureDeps = {
    cacheGet: key => withStore<Blob | undefined>("readonly", s => s.get(key)),
    cachePut: (key, blob) => withStore<void>("readwrite", s => s.put(blob, key)),
    fetch: async url => {
        const r = await fetch(url, {mode: "cors"});
        return r.ok ? await r.blob() : undefined;
    },
    size: src => new Promise(resolve => {
        const img = new Image();
        img.onload = () => resolve(img.naturalWidth > 0 ? {width: img.naturalWidth, height: img.naturalHeight} : undefined);
        img.onerror = () => resolve(undefined);
        img.src = src;
    }),
    objectUrl: blob => URL.createObjectURL(blob),
};

// Pictures found in this page, by url and copy: each is looked for once.
const found = new Map<string, Promise<Loaded | undefined>>();

export function picture(pic: EnvPicture | undefined): Promise<Loaded | undefined> {
    if (pic === undefined || (pic.url === undefined && pic.copy === undefined))
        return Promise.resolve(undefined);
    const key = JSON.stringify([pic.url ?? "", pic.copy ?? ""]);
    let p = found.get(key);
    if (p === undefined) {
        p = loadPicture(pic, browserDeps);
        found.set(key, p);
    }
    return p;
}
