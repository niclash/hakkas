import {describe, expect, it} from "vitest";
import {
    bindingsFileText, channelLabel, channelProblem, channelsOf, CheckContext, checkEntries, initialRows, parseBindingsFile,
    parseChannel, rowEntry, rowProblems
} from "@/components/bindings";
import type {EnvDescription} from "@/components/protocols/environment";

// 06 §4.14: the environment's channels, the checks of binding a block port (the engine's, 04 §4),
// the bindings file's form and the mapping dialog's rows.

const desc: EnvDescription = {
    name: "sim",
    outPorts: [{id: "ain", type: "number", addressable: true, count: 2}, {id: "temp", type: "number", unit: "°C"}],
    inPorts: [{id: "relay", type: "bool", addressable: true, count: 2}],
    modules: [],
};

const ctx = (over: Partial<CheckContext> = {}): CheckContext => ({
    description: desc,
    blocks: {a: {inputs: ["in1", "in2"], outputs: ["out"]}, b: {inputs: ["in1"], outputs: ["out"]}},
    sourced: new Set(["b.in1"]),
    bindings: [],
    graph: "p",
    ...over,
});

describe("channels", () => {
    it("lists every channel, sensors first; labels and their parsing", () => {
        expect(channelsOf(desc).map(c => c.label)).toEqual(["ain[1]", "ain[2]", "temp", "relay[1]", "relay[2]"]);
        expect(channelsOf(desc).map(c => c.sensor)).toEqual([true, true, true, false, false]);
        expect(channelLabel("ain", 2)).toBe("ain[2]");
        expect(channelLabel("temp")).toBe("temp");
        expect(parseChannel("ain[2]")).toEqual({env: "ain", index: 2});
        expect(parseChannel("temp")).toEqual({env: "temp"});
    });

    it("says what is wrong with a channel, in the engine's words", () => {
        expect(channelProblem(desc, "ain", 1)).toBeUndefined();
        expect(channelProblem(desc, "nope")).toBe("Unknown environment port: nope");
        expect(channelProblem(desc, "ain")).toBe("Index required: ain");
        expect(channelProblem(desc, "temp", 1)).toBe("No index expected: temp");
        expect(channelProblem(desc, "ain", 3)).toBe("Index out of range: ain[3]");
    });
});

describe("checkEntries", () => {
    it("warns as the engine would skip, each entry against the earlier ones", () => {
        const w = checkEntries([
            {block: "a", port: "in1", env: "ain", index: 1},     // made
            {block: "a", port: "in1", env: "temp"},              // a second source
            {block: "a", port: "out", env: "relay", index: 1},   // made
            {block: "b", port: "out", env: "relay", index: 1},   // a second driver
            {block: "b", port: "in1", env: "temp"},              // has a connection
            {block: "z", port: "in1", env: "temp"},
            {block: "a", port: "zz", env: "temp"},
            {block: "a", port: "out", env: "temp"},
            {block: "a", port: "in2", env: "relay", index: 2},
            {block: "a", port: "in2", env: "ain", index: 9},
            {block: "", port: "in2", env: "temp"},
        ], ctx());
        expect(w).toEqual([undefined, "Input already connected: a.in1", undefined, "Actuator already driven: relay[1]",
            "Input already connected: b.in1", "Unknown node: z", "Unknown port: a.zz", "Invalid binding direction",
            "Invalid binding direction", "Index out of range: ain[9]", "Invalid id"]);
    });

    it("counts what stays bound: another process's driver; this process's inputs", () => {
        const c = ctx({bindings: [{graph: "q", block: "x", port: "out", env: "relay", index: 2},
            {graph: "p", block: "a", port: "in2", env: "temp"}]});
        expect(checkEntries([{block: "a", port: "out", env: "relay", index: 2}, {block: "a", port: "in2", env: "ain", index: 1},
            {block: "a", port: "in1", env: "ain", index: 1}], c))
            .toEqual(["Actuator already driven: relay[2]", "Input already connected: a.in2", undefined]);
    });
});

describe("bindings files", () => {
    it("reads the engine's form and writes it back", () => {
        const text = bindingsFileText("sim", [{graph: "p", block: "a", port: "in1", env: "ain", index: 1},
            {graph: "p", block: "a", port: "out", env: "temp"}]);
        expect(JSON.parse(text)).toEqual({environment: "sim", bindings: [{block: "a", port: "in1", env: "ain", index: 1},
            {block: "a", port: "out", env: "temp"}]});
        expect(parseBindingsFile(text)).toEqual({environment: "sim", bindings: [{block: "a", port: "in1", env: "ain", index: 1},
            {block: "a", port: "out", env: "temp"}]});
        expect(parseBindingsFile('{"environment":"e","bindings":[{"block":"a","port":"p","env":"x","index":null}]}'))
            .toEqual({environment: "e", bindings: [{block: "a", port: "p", env: "x"}]});
    });

    it("says what is wrong with a file that is not one", () => {
        expect(parseBindingsFile("{")).toBe("Not a bindings file: not JSON");
        expect(parseBindingsFile("[]")).toBe("Not a bindings file: expected an object");
        expect(parseBindingsFile('{"bindings":[]}')).toBe("environment: expected a non-empty string");
        expect(parseBindingsFile('{"environment":"e","bindings":{}}')).toBe("bindings: expected an array");
        expect(parseBindingsFile('{"environment":"e","bindings":[{"block":"a","port":"p"}]}'))
            .toBe("bindings[0]: env: expected a non-empty string");
        expect(parseBindingsFile('{"environment":"e","bindings":[{"block":"a","port":"p","env":"x","index":0}]}'))
            .toBe("bindings[0]: index: expected an integer of at least 1");
    });
});

describe("the mapping dialog's rows", () => {
    it("keeps what exists, leaves the rest to choose, and checks the choices", () => {
        const c = ctx();
        const rows = initialRows([{block: "a", port: "in1", env: "ain", index: 3}, {block: "gone", port: "in1", env: "temp"},
            {block: "a", port: "out", env: "relay", index: 1}], c);
        expect(rows.map(r => [r.blockPort, r.channel])).toEqual([["a.in1", ""], ["", "temp"], ["a.out", "relay[1]"]]);
        expect(rowProblems(rows, c)).toEqual(["Choose a channel", "Choose a block port", undefined]);
        rows[0].channel = "ain[2]";
        rows[1].blockPort = "a.in1";
        expect(rowProblems(rows, c)).toEqual([undefined, "Input already connected: a.in1", undefined]);
        rows[1].skip = true;
        expect(rowProblems(rows, c)).toEqual([undefined, undefined, undefined]);
        expect(rowEntry(rows[0])).toEqual({block: "a", port: "in1", env: "ain", index: 2});
    });
});
