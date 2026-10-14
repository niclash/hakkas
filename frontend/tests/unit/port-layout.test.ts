import {describe, expect, it} from "vitest";
import {layoutPorts, PORT_GAP, PORT_SPACING} from "@/components/floweditor/port-layout";

// 06 §4.8: process port markers at the process's edges, level with their ports, never overlapping.

describe("layoutPorts", () => {
    const content = [{x: 100, y: 0, width: 200, height: 300}, {x: 400, y: 50, width: 180, height: 100}];
    const m = (id: string, direction: "in" | "out", socketY: number) => ({id, direction, socketY, width: 130, height: 30});

    it("inputs left of everything, outputs right of it, level with their ports", () => {
        const at = layoutPorts(content, [m("a", "in", 100), m("b", "out", 75)]);
        expect(at.get("a")).toEqual({x: 100 - PORT_GAP - 130, y: 85});
        expect(at.get("b")).toEqual({x: 580 + PORT_GAP, y: 60});
    });

    it("markers that would overlap are moved down, in their ports' order", () => {
        const at = layoutPorts(content, [m("b", "in", 110), m("a", "in", 100), m("c", "out", 100)]);
        expect(at.get("a")!.y).toBe(85);
        expect(at.get("b")!.y).toBe(85 + 30 + PORT_SPACING);
        expect(at.get("c")!.y).toBe(85);
    });

    it("nothing to place against", () => {
        expect(layoutPorts([], [m("a", "in", 0)]).size).toBe(0);
    });
});
