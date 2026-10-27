import {describe, expect, it} from "vitest";
import {addLink, addPort, addProcess, emptyMap, layoutMap, linkKey, loadLinks, removeLink, removePort, removeProcess, renamePort, stepHolds} from "@/components/map/state";

// 06 §4.17: the process map's model, fed by link:links, link:add/remove and the port events.

const link = (a: string, o: string, b: string, i: string) => ({src: {graph: a, port: o}, tgt: {graph: b, port: i}});

describe("the process map's model", () => {
    it("loads link:links and follows links, idempotently", () => {
        const s = emptyMap();
        loadLinks(s, [link("p", "o", "q", "i")], [{graph: "p", inports: [], outports: ["o"]}, {graph: "q", inports: ["i"], outports: []}]);
        expect([...s.processes.keys()]).toEqual(["p", "q"]);
        addLink(s, link("p", "o", "q", "i"));
        expect(s.links).toHaveLength(1);
        addLink(s, link("q", "x", "p", "y"));
        removeLink(s, link("p", "o", "q", "i"));
        removeLink(s, link("p", "o", "q", "i"));
        expect(s.links).toEqual([link("q", "x", "p", "y")]);
        expect(linkKey(link("a.b", "c", "d", "e"))).not.toBe(linkKey(link("a", "b.c", "d", "e")));
    });

    it("follows processes and their ports", () => {
        const s = emptyMap();
        addProcess(s, "p");
        addPort(s, "p", "in", "a");
        addPort(s, "p", "in", "a");
        addPort(s, "p", "out", "b");
        addPort(s, "user/T", "in", "x");      // a component graph's: not on the map
        expect(s.processes.get("p")).toEqual({inports: ["a"], outports: ["b"]});
        expect(s.processes.has("user/T")).toBe(false);
        renamePort(s, "p", "in", "a", "c");
        renamePort(s, "p", "in", "a", "d");   // already renamed
        removePort(s, "p", "out", "b");
        removePort(s, "p", "out", "b");
        expect(s.processes.get("p")).toEqual({inports: ["c"], outports: []});
        addLink(s, link("p", "x", "q", "y"));
        removeProcess(s, "p");
        expect(s.processes.has("p")).toBe(false);
        expect(s.links).toEqual([]);
    });

    it("checks a step before undoing or redoing it", () => {
        const s = emptyMap();
        const l = link("p", "o", "q", "i");
        addLink(s, l);
        expect(stepHolds(s, {add: true, link: l}, true)).toBe(true);    // undo an add: the link is there
        expect(stepHolds(s, {add: false, link: l}, true)).toBe(false);  // someone made it again
        removeLink(s, l);
        expect(stepHolds(s, {add: true, link: l}, false)).toBe(true);   // redo the add: it is gone
        expect(stepHolds(s, {add: false, link: l}, true)).toBe(true);   // undo a remove: it is gone
    });

    it("lays the processes out in columns along the links", () => {
        const s = emptyMap();
        for (const g of ["a", "b", "c", "d", "e"])
            addProcess(s, g);
        addLink(s, link("a", "o", "b", "i"));
        addLink(s, link("b", "o", "c", "i"));
        addLink(s, link("a", "o", "c", "j"));
        addLink(s, link("d", "o", "d", "i"));     // to itself: no column of its own
        const at = layoutMap(s, g => ({width: g === "b" ? 400 : 300, height: 100}));
        expect(Object.fromEntries(at)).toEqual({
            a: {x: 0, y: 0}, d: {x: 0, y: 148}, e: {x: 0, y: 296},      // fed by no other process
            b: {x: 460, y: 0},                                          // columns 160 apart
            c: {x: 460 + 400 + 160, y: 0},                              // as wide as their widest
        });
        // A loop of links is cut somewhere, and every process placed.
        addLink(s, link("c", "o", "a", "k"));
        expect(layoutMap(s, () => ({width: 300, height: 100})).size).toBe(5);
    });
});
