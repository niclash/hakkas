import {describe, expect, it} from "vitest";
import {BlockSelection, MoveThrottle, Position, SelectionDrag} from "@/components/floweditor/selection/blocks";

// 06 §4: the selection is the view's own state; moves of a selection go to the engine.

function tracked() {
    const changes: string[] = [];
    const selection = new BlockSelection((id, selected) => changes.push((selected ? "+" : "-") + id));
    return {selection, changes};
}

describe("BlockSelection", () => {
    it("a click selects one block", () => {
        const {selection, changes} = tracked();
        selection.press("a", false);
        selection.release(false);
        selection.press("b", false);
        selection.release(false);
        expect(selection.ids()).toEqual(["b"]);
        expect(changes).toEqual(["+a", "-a", "+b"]);
    });

    it("Ctrl/Shift-click adds a block, and unselects a selected one", () => {
        const {selection} = tracked();
        selection.press("a", false);
        selection.release(false);
        selection.press("b", true);
        selection.release(false);
        selection.press("c", true);
        selection.release(false);
        expect(selection.ids()).toEqual(["a", "b", "c"]);
        selection.press("b", true);
        selection.release(false);
        expect(selection.ids()).toEqual(["a", "c"]);
    });

    it("pressing a selected block keeps the selection, so a drag moves them all", () => {
        const {selection, changes} = tracked();
        selection.set(["a", "b"]);
        changes.length = 0;
        selection.press("a", false);
        expect(selection.ids()).toEqual(["a", "b"]);
        selection.release(true);
        expect(selection.ids()).toEqual(["a", "b"]);
        // Even with Ctrl/Shift, a drag does not unselect it.
        selection.press("b", true);
        selection.release(true);
        expect(selection.ids()).toEqual(["a", "b"]);
        expect(changes).toEqual([]);
    });

    it("a click on a block of the selection selects it alone", () => {
        const {selection} = tracked();
        selection.set(["a", "b", "c"]);
        selection.press("b", false);
        selection.release(false);
        expect(selection.ids()).toEqual(["b"]);
    });

    it("set, add, clear and remove report only changes", () => {
        const {selection, changes} = tracked();
        selection.add(["a", "b"]);
        selection.add(["b", "c"]);
        selection.set(["c", "d"]);
        selection.remove("x");
        selection.clear();
        expect(changes).toEqual(["+a", "+b", "+c", "-a", "-b", "+d", "-c", "-d"]);
        expect(selection.ids()).toEqual([]);
    });

    it("a block that is gone leaves the selection without a change", () => {
        const {selection, changes} = tracked();
        selection.set(["a", "b"]);
        changes.length = 0;
        selection.press("a", false);
        selection.forget("a");
        selection.release(false);
        expect(selection.ids()).toEqual(["b"]);
        expect(changes).toEqual([]);
    });
});

describe("SelectionDrag", () => {
    it("every selected block keeps its offset from the dragged one", () => {
        const drag = new SelectionDrag("a", new Map([["a", {x: 10, y: 20}], ["b", {x: 100, y: -5}]]));
        expect(drag.positions({x: 15, y: 50})).toEqual(new Map([["a", {x: 15, y: 50}], ["b", {x: 105, y: 25}]]));
        expect(drag.positions({x: 10, y: 20})).toEqual(new Map([["a", {x: 10, y: 20}], ["b", {x: 100, y: -5}]]));
    });
});

describe("MoveThrottle", () => {
    function throttle() {
        const sent: [string, Position][] = [];
        const frames: (() => void)[] = [];
        let cancelled = 0;
        const t = new MoveThrottle((id, p) => sent.push([id, p]),
            callback => frames.push(callback), () => cancelled++);
        return {t, sent, frames, cancelled: () => cancelled};
    }

    it("sends the latest position per block once per frame", () => {
        const {t, sent, frames} = throttle();
        t.move("a", {x: 1, y: 1});
        t.move("b", {x: 5, y: 5});
        t.move("a", {x: 2, y: 2});
        expect(sent).toEqual([]);
        expect(frames).toHaveLength(1);
        frames[0]();
        expect(sent).toEqual([["a", {x: 2, y: 2}], ["b", {x: 5, y: 5}]]);
        t.move("a", {x: 3, y: 3});
        expect(frames).toHaveLength(2);
        frames[1]();
        expect(sent).toHaveLength(3);
    });

    it("flush sends the final positions at once", () => {
        const {t, sent, frames, cancelled} = throttle();
        t.move("a", {x: 1, y: 1});
        t.flush();
        expect(sent).toEqual([["a", {x: 1, y: 1}]]);
        expect(cancelled()).toBe(1);
        t.flush();
        expect(sent).toHaveLength(1);
        expect(cancelled()).toBe(1);
        // A new move after the flush schedules a new frame.
        t.move("a", {x: 2, y: 2});
        expect(frames).toHaveLength(2);
    });
});
