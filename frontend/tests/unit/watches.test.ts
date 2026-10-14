import {describe, expect, it} from "vitest";
import {formatLive, pointKey, Watch, Watches} from "@/components/watches";

// 06 §4.7: the set of watched inputs sent as network:edges, and how a live value is shown.

const w = (src: string, tgt: string): Watch => {
    const [sn, sp] = src.split("."), [tn, tp] = tgt.split(".");
    return {src: {node: sn, port: sp}, tgt: {node: tn, port: tp}};
};

describe("Watches", () => {
    it("explicit watches, by input; watching an input again replaces it", () => {
        const ws = new Watches();
        ws.add(w("a.out", "b.in1"));
        ws.add(w("b.in2", "b.in2"));
        ws.add(w("c.out", "b.in1"));
        expect(ws.request([])).toEqual([w("c.out", "b.in1"), w("b.in2", "b.in2")]);
        ws.remove({node: "b", port: "in1"});
        expect(ws.has({node: "b", port: "in1"})).toBe(false);
        expect(ws.request([])).toEqual([w("b.in2", "b.in2")]);
    });

    it("a connection now feeding a watched input gives its source", () => {
        const ws = new Watches();
        ws.add(w("b.in2", "b.in2"));
        expect(ws.request([w("x.out", "b.in2")])).toEqual([w("x.out", "b.in2")]);
    });

    it("watch all adds every connection not already watched", () => {
        const ws = new Watches();
        ws.add(w("a.out", "b.in1"));
        ws.all = true;
        expect(ws.request([w("a.out", "b.in1"), w("b.out", "c.in")])).toEqual([w("a.out", "b.in1"), w("b.out", "c.in")]);
        ws.clear();
        expect(ws.all).toBe(false);
        expect(ws.request([w("b.out", "c.in")])).toEqual([]);
    });

    it("renamed and removed blocks", () => {
        const ws = new Watches();
        ws.add(w("a.out", "b.in1"));
        ws.add(w("b.out", "c.in"));
        ws.renameBlock("b", "bee");
        expect(ws.request([])).toEqual([w("a.out", "bee.in1"), w("bee.out", "c.in")]);
        ws.removeBlock("bee");
        expect(ws.request([])).toEqual([w("bee.out", "c.in")]);
    });

    it("keys tell apart ids with dots", () => {
        expect(pointKey({node: "a.b", port: "c"})).not.toBe(pointKey({node: "a", port: "b.c"}));
    });
});

describe("formatLive", () => {
    it("numbers with at most four decimals, bools as they are, text quoted and cut", () => {
        expect(formatLive("number", "12")).toBe("12");
        expect(formatLive("number", "3.14159265")).toBe("3.1416");
        expect(formatLive("number", "1e999")).toBe("1e999");
        expect(formatLive("", "-0.5")).toBe("-0.5");
        expect(formatLive("bool", "true")).toBe("true");
        expect(formatLive("string", "08:00-17:00")).toBe("“08:00-17:00”");
        expect(formatLive("string", "x".repeat(30))).toBe("“" + "x".repeat(23) + "…”");
    });
});
