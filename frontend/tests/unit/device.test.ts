import {describe, expect, it} from "vitest";
import {cropOf, drawing, loadPicture, Loaded, PictureDeps, rotationOf, shownSize} from "@/components/pictures";
import {badgeText, layout, valueKey, valueText} from "@/components/device";
import type {EnvDescription} from "@/components/protocols/environment";

// 06 §4.15 (plan task 78): where a picture comes from, how it is drawn, what goes where.

function deps(o: {cached?: string[], cors?: string[], images?: string[]}, log: string[] = []): PictureDeps {
    const cache = new Map((o.cached ?? []).map(k => [k, new Blob([k])]));
    return {
        cacheGet: async k => cache.get(k),
        cachePut: async (k, b) => {
            log.push("put " + k);
            cache.set(k, b);
        },
        fetch: async url => {
            log.push("fetch " + url);
            return o.cors?.includes(url) ? new Blob([url]) : undefined;
        },
        size: async src => (o.images ?? []).includes(src) || src.startsWith("blob:") ? {width: 200, height: 100} : undefined,
        objectUrl: b => "blob:" + b.size,
    };
}

describe("a picture's source", () => {
    const url = "https://vendor.example/aiv.png";

    it("is the cache first, then the url (cached when CORS allows), else the url as it is", async () => {
        expect(await loadPicture({url}, deps({cached: [url]}))).toMatchObject({src: "blob:" + url.length, copy: false});
        const log: string[] = [];
        expect(await loadPicture({url}, deps({cors: [url]}, log))).toMatchObject({src: expect.stringMatching(/^blob:/)});
        expect(log).toEqual(["fetch " + url, "put " + url]);
        expect(await loadPicture({url}, deps({images: [url]}))).toEqual({src: url, width: 200, height: 100, copy: false});
    });

    it("is the device's copy when the url gives no image, or there is none", async () => {
        const copy = "/environment/pictures/colibri_aiv.webp";
        expect(await loadPicture({url, copy: "colibri_aiv.webp"}, deps({images: [copy]})))
            .toEqual({src: copy, width: 200, height: 100, copy: true});
        expect(await loadPicture({copy: "colibri_aiv.webp"}, deps({images: [copy]}))).toMatchObject({copy: true});
        expect(await loadPicture({url, copy: "x.webp"}, deps({}))).toBeUndefined();
    });

    it("never loads an http url or a copy that is a path", async () => {
        const log: string[] = [];
        expect(await loadPicture({url: "http://vendor.example/a.png"}, deps({images: ["http://vendor.example/a.png"]}, log)))
            .toBeUndefined();
        expect(log).toEqual([]);
        expect(await loadPicture({copy: "../secret"}, deps({images: ["/environment/pictures/..%2Fsecret"]})))
            .toBeUndefined();
    });
});

describe("a picture as shown", () => {
    const file: Loaded = {src: "s", width: 1904, height: 1064, copy: false};
    const mcu = {url: "u", rotation: 90, crop: {x: 0.25, y: 0.25, w: 0.5, h: 0.5}};

    it("is cropped, then turned clockwise", () => {
        expect(shownSize(mcu, file)).toEqual({width: 532, height: 952});
        expect(shownSize({url: "u"}, file)).toEqual({width: 1904, height: 1064});
        expect(drawing(mcu, file, {x: 100, y: 200, w: 50, h: 100})).toEqual({
            transform: "translate(125 250) rotate(90)", box: {x: -50, y: -25, w: 100, h: 50}, viewBox: "0.25 0.25 0.5 0.5",
        });
    });

    it("ignores the crop and rotation for a copy, and ones that do not fit", () => {
        const copy = {...file, copy: true};
        expect(rotationOf(mcu, copy)).toBe(0);
        expect(cropOf(mcu, copy)).toEqual({x: 0, y: 0, w: 1, h: 1});
        expect(rotationOf({url: "u", rotation: 45}, file)).toBe(0);
        expect(cropOf({url: "u", crop: {x: 0.6, y: 0, w: 0.5, h: 1}}, file)).toEqual({x: 0, y: 0, w: 1, h: 1});
    });
});

describe("the device's layout", () => {
    const rect = {x: 0.5, y: 0.4, w: 0.1, h: 0.2};
    const d: EnvDescription = {
        name: "lab",
        outPorts: [{id: "slot1-ai", type: "number", addressable: true, count: 2, unit: "V"},
            {id: "slot0-di", type: "bool"}, {id: "temp", type: "number"}],
        inPorts: [{id: "slot2-dq", type: "bool"}, {id: "slot1-k", type: "number"}],
        modules: [
            {slot: 0, builtin: true, type: "carrier", ports: ["slot0-di"], positions: [{port: "slot0-di", x: 0.9, y: 0.8}]},
            {slot: 1, type: "aiv", name: "AIV", documentation: "https://doc", picture: {url: "https://p"},
                ports: ["slot1-ai", "slot1-k"], positions: [
                    {port: "slot1-ai", index: 1, x: 0.5, y: 0.5},
                    {port: "slot1-ai", index: 3, x: 0.5, y: 0.5},       // no such channel
                    {port: "slot1-ai", x: 0.5, y: 0.5},                 // an index is needed
                    {port: "slot1-ai", index: 2, x: 1.5, y: 0.5},       // outside the picture
                    {port: "nope", x: 0, y: 0},
                ]},
            {slot: 2, type: "ssr", ports: ["slot2-dq"], positions: [{port: "slot2-dq", x: 0, y: 1}]},
            {slot: 5, type: "lost", ports: []},                          // no place: not drawn
        ],
        carrier: {type: "c8", slots: 7, builtin: [{slot: 0}], comm: [{slot: 1, type: "eth"}], mcu: {type: "mcu6"},
            places: {slots: [{slot: 1, ...rect}, {slot: 2, x: 0.6, y: 0.4, w: 0.1, h: 0.2}, {slot: 0, ...rect}],
                comm: [{slot: 1, x: 0.1, y: 0.4, w: 0.1, h: 0.2}], mcu: {x: 0.2, y: 0.4, w: 0.1, h: 0.2}}},
    };

    it("puts each module, the communication and MCU modules into their places", () => {
        const l = layout(d);
        expect(l.parts.map(p => [p.key, p.kind, p.label])).toEqual([
            ["m1", "module", "AIV"], ["m2", "module", "ssr"], ["c1", "comm", "eth"], ["mcu", "mcu", "mcu6"]]);
        expect(l.parts[0]).toMatchObject({rect, documentation: "https://doc", title: "Slot 1: AIV · aiv"});
    });

    it("puts each usable position on the carrier's picture, a built-in module's as it is", () => {
        const l = layout(d);
        expect(l.badges.map(b => [valueKey(b.env, b.index), b.sensor, b.x, b.y])).toEqual([
            ["slot0-di[0]", true, 0.9, 0.8],
            ["slot1-ai[1]", true, 0.55, 0.5],
            ["slot2-dq[0]", false, 0.6, 0.6000000000000001],
        ]);
    });

    it("stacks the badges at one point", () => {
        const twice: EnvDescription = {...d, modules: [{slot: 2, type: "dio", ports: ["slot2-dq", "slot0-di"], positions: [
            {port: "slot2-dq", x: 0.5, y: 0.5}, {port: "slot0-di", x: 0.5, y: 0.5}]}]};
        expect(layout(twice).badges.map(b => b.stack)).toEqual([0, 1]);
    });

    it("lists the channels with no point, by module, then those of no module", () => {
        expect(layout(d).others.map(o => [o.label, o.channels.map(c => valueKey(c.env, c.index))])).toEqual([
            ["Slot 1: AIV", ["slot1-ai[2]", "slot1-k[0]"]],
            ["Other ports", ["temp[0]"]],
        ]);
    });
});

describe("a value as shown", () => {
    it("has up to three decimals and its unit; on, off, or none", () => {
        expect(valueText(3.21456, "V")).toBe("3.215 V");
        expect(valueText(2)).toBe("2");
        expect(valueText(true)).toBe("on");
        expect(valueText(false)).toBe("off");
        expect(valueText(undefined)).toBe("–");
        expect(valueText(null)).toBe("NaN");
    });

    it("on a badge, has three significant digits and no unit", () => {
        expect(badgeText(21.37)).toBe("21.4");
        expect(badgeText(3.214)).toBe("3.21");
        expect(badgeText(0.00512)).toBe("0.00512");
        expect(badgeText(12345.6)).toBe("12346");
        expect(badgeText(undefined)).toBe("–");
    });
});
