import {describe, expect, it} from "vitest";
import {menuFor, nextIndex, searchTypes} from "@/components/floweditor/menus";

// 06 §4.4: what each context menu offers, the block search, and moving through a menu.

const actions = (target: Parameters<typeof menuFor>[0]) => menuFor(target).map(i => i.action);

describe("menuFor", () => {
    it("the canvas", () => {
        expect(actions({kind: "canvas"})).toEqual(["add-block", "select-all", "zoom-fit", "arrange", "watch-all"]);
        expect(actions({kind: "canvas", canPaste: true})).toEqual(["add-block", "paste", "select-all", "zoom-fit", "arrange", "watch-all"]);
        expect(actions({kind: "canvas", watchingAll: true})).toContain("unwatch-all");
    });

    it("a block, alone or in a selection", () => {
        expect(actions({kind: "block", id: "a", selected: 0}))
            .toEqual(["rename", "edit-inputs", "watch-inputs", "copy", "duplicate", "create-composite", "delete-block"]);
        expect(actions({kind: "block", id: "a", selected: 1, watched: true}))
            .toEqual(["rename", "edit-inputs", "unwatch-inputs", "copy", "duplicate", "create-composite", "delete-block"]);
        const many = menuFor({kind: "block", id: "a", selected: 3});
        expect(many.map(i => i.action))
            .toEqual(["rename", "edit-inputs", "watch-inputs", "copy", "duplicate", "create-composite", "delete-selection"]);
        expect(many[3].label).toBe("Copy 3 blocks");
        expect(many[5].label).toBe("Create composite type from 3 blocks…");
        expect(many[6].label).toBe("Delete 3 blocks");
    });

    it("an input, by what feeds it", () => {
        expect(actions({kind: "input", id: "a", port: "in", source: {kind: "default"}})).toEqual(["set-value", "watch", "expose-input"]);
        expect(actions({kind: "input", id: "a", port: "in", source: {kind: "fixed", value: "1"}, watched: true}))
            .toEqual(["change-value", "clear-value", "unwatch", "expose-input"]);
        expect(actions({kind: "input", id: "a", port: "in", source: {kind: "connected", from: "b.out"}}))
            .toEqual(["disconnect", "watch", "expose-input"]);
    });

    it("a value node and a connection", () => {
        expect(actions({kind: "value", id: "a", port: "in"})).toEqual(["change-value", "clear-value", "watch"]);
        expect(actions({kind: "connection"})).toEqual(["delete-connection", "watch"]);
        expect(actions({kind: "connection", watched: true})).toEqual(["delete-connection", "unwatch"]);
        expect(actions({kind: "output", id: "a", port: "out"})).toEqual(["expose-output"]);
        expect(actions({kind: "port", direction: "in", name: "x"})).toEqual(["rename-port", "remove-port"]);
    });

    // 06 §4.14: binding to the environment, when there is one.
    it("binding: a free input or an output binds, a bound one unbinds", () => {
        expect(actions({kind: "input", id: "a", port: "in", source: {kind: "default"}, environment: true}))
            .toEqual(["set-value", "bind", "watch", "expose-input"]);
        expect(actions({kind: "input", id: "a", port: "in", source: {kind: "fixed", value: "1"}, environment: true}))
            .not.toContain("bind");
        const bound = menuFor({kind: "input", id: "a", port: "in", source: {kind: "bound", channel: "ain[2]"}, environment: true});
        expect(bound.map(i => i.action)).toEqual(["unbind", "watch", "expose-input"]);
        expect(bound[0].label).toBe("Unbind from ain[2]");
        expect(actions({kind: "output", id: "a", port: "out", environment: true})).toEqual(["bind", "expose-output"]);
        expect(actions({kind: "output", id: "a", port: "out", environment: true, binding: "relay[1]"}))
            .toEqual(["unbind", "expose-output"]);
        expect(menuFor({kind: "binding", id: "a", port: "out", channel: "relay[1]"})[0].label).toBe("Unbind a.out from relay[1]");
    });
});

describe("searchTypes", () => {
    const names = ["math/Add2", "math/Add4", "compare/Greater", "process/Deadband", "timing/DayOfWeek"];

    it("everything for no text; names starting with it first, then names containing it", () => {
        expect(searchTypes(names, " ")).toEqual(names);
        expect(searchTypes(names, "add")).toEqual(["math/Add2", "math/Add4"]);
        expect(searchTypes(["x/Greater", "y/Eat"], "eat")).toEqual(["y/Eat", "x/Greater"]);
        expect(searchTypes(names, "DAY")).toEqual(["timing/DayOfWeek"]);
        expect(searchTypes(names, "math")).toEqual(["math/Add2", "math/Add4"]);
        expect(searchTypes(names, "zzz")).toEqual([]);
    });
});

describe("nextIndex", () => {
    it("moves and wraps", () => {
        expect(nextIndex(0, 3, "ArrowDown")).toBe(1);
        expect(nextIndex(2, 3, "ArrowDown")).toBe(0);
        expect(nextIndex(0, 3, "ArrowUp")).toBe(2);
        expect(nextIndex(-1, 3, "ArrowUp")).toBe(2);
        expect(nextIndex(1, 3, "Home")).toBe(0);
        expect(nextIndex(1, 3, "End")).toBe(2);
        expect(nextIndex(0, 0, "ArrowDown")).toBe(-1);
    });
});
