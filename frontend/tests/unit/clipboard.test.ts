import {describe, expect, it} from "vitest";
import {ClipBlock, clipOrigin, freeId, makeClip, parseClip, pasteCommands} from "@/components/clipboard";

// 06 §4.10: copy, paste and duplicate.

const blocks: ClipBlock[] = [
    {id: "a", component: "math/Add2", x: 100, y: 50, initials: {in2: "5"}},
    {id: "Add2_1", component: "math/Add2", x: 300, y: 80, initials: {}},
];
const links = [
    {src: {node: "a", port: "out"}, tgt: {node: "Add2_1", port: "in1"}},
    {src: {node: "x", port: "out"}, tgt: {node: "a", port: "in1"}},
];

describe("clips", () => {
    it("only the connections between copied blocks", () => {
        expect(makeClip(blocks, links).links).toEqual([links[0]]);
    });

    it("round-trips as JSON, and other text is no clip", () => {
        const clip = makeClip(blocks, links);
        expect(parseClip(JSON.stringify(clip))).toEqual(clip);
        expect(parseClip("hello")).toBeUndefined();
        expect(parseClip("{}")).toBeUndefined();
        expect(parseClip(JSON.stringify({...clip, version: 2}))).toBeUndefined();
        expect(parseClip(JSON.stringify({...clip, blocks: [{id: "a"}]}))).toBeUndefined();
        expect(parseClip(JSON.stringify({...clip, links: [{src: {node: 1}}]}))).toBeUndefined();
    });

    it("the clip's top-left corner", () => {
        expect(clipOrigin(makeClip(blocks, []))).toEqual({x: 100, y: 50});
    });
});

describe("pasting", () => {
    it("free names: the own one, else the base with the first free number", () => {
        expect(freeId("a", new Set())).toBe("a");
        expect(freeId("a", new Set(["a"]))).toBe("a_1");
        expect(freeId("Add2_1", new Set(["Add2_1", "Add2_2"]))).toBe("Add2_3");
    });

    it("blocks moved, then connections and fixed values, with the new names", () => {
        const p = pasteCommands(makeClip(blocks, links), "g", new Set(["a", "Add2_1"]), 40, 40);
        expect([...p.ids]).toEqual([["a", "a_1"], ["Add2_1", "Add2_2"]]);
        expect(p.commands).toEqual([
            {command: "addnode", payload: {graph: "g", id: "a_1", component: "math/Add2", metadata: {x: 140, y: 90}}},
            {command: "addnode", payload: {graph: "g", id: "Add2_2", component: "math/Add2", metadata: {x: 340, y: 120}}},
            {command: "addedge", payload: {graph: "g", src: {node: "a_1", port: "out"}, tgt: {node: "Add2_2", port: "in1"}}},
            {command: "addinitial", payload: {graph: "g", tgt: {node: "a_1", port: "in2"}, src: {data: "5"}}},
        ]);
    });

    it("into another process, the names are kept when free", () => {
        const p = pasteCommands(makeClip(blocks, links), "h", new Set(), 0, 0);
        expect([...p.ids.values()]).toEqual(["a", "Add2_1"]);
    });
});
