import {describe, expect, it} from "vitest";
import {
    bindingsFileText, channelLabel, channelProblem, channelsOf, CheckContext, checkEntries, initialRows, parseBindingsFile,
    parseChannel, rowEntry, rowProblems
} from "@/components/bindings";
import type {EnvDescription} from "@/components/protocols/environment";

// 06 §4.14: the environment's channels, the checks of binding a process port (the engine's,
// 04 §4; plan task 70), the bindings file's form and the mapping dialog's rows.

const desc: EnvDescription = {
    name: "sim",
    outPorts: [{id: "ain", type: "number", addressable: true, count: 2}, {id: "temp", type: "number", unit: "°C"}],
    inPorts: [{id: "relay", type: "bool", addressable: true, count: 2}],
    modules: [],
};

// The process's ports: inports level (a.in1), flow (a.in2), fed (b.in1, which has a
// connection) and x (also a.in1); outports valve (a.out), x (b.out).
const ctx = (over: Partial<CheckContext> = {}): CheckContext => ({
    description: desc,
    inports: {level: "a.in1", flow: "a.in2", fed: "b.in1", x: "a.in1"},
    outports: {valve: "a.out", x: "b.out"},
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
            {port: "level", env: "ain", index: 1},     // made
            {port: "level", env: "temp"},              // a second source
            {port: "x", env: "temp"},                  // the same input, through another inport
            {port: "valve", env: "relay", index: 1},   // made
            {port: "x", env: "relay", index: 1},       // a second driver (the outport x)
            {port: "fed", env: "temp"},                // has a connection
            {port: "zz", env: "temp"},
            {port: "valve", env: "temp"},              // only an outport has the name
            {port: "flow", env: "relay", index: 2},    // only an inport has it
            {port: "flow", env: "ain", index: 9},
            {port: "", env: "temp"},
        ], ctx());
        expect(w).toEqual([undefined, "Input already connected: level", "Input already connected: x", undefined,
            "Actuator already driven: relay[1]", "Input already connected: fed", "Unknown port: zz",
            "Invalid binding direction", "Invalid binding direction", "Index out of range: ain[9]", "Invalid id"]);
    });

    it("counts what stays bound: another process's driver; this process's inputs", () => {
        const c = ctx({bindings: [{graph: "q", port: "out", env: "relay", index: 2}, {graph: "p", port: "flow", env: "temp"}]});
        expect(checkEntries([{port: "valve", env: "relay", index: 2}, {port: "flow", env: "ain", index: 1},
            {port: "level", env: "ain", index: 1}], c))
            .toEqual(["Actuator already driven: relay[2]", "Input already connected: flow", undefined]);
    });

    it("lets an outport drive several actuators", () => {
        expect(checkEntries([{port: "valve", env: "relay", index: 1}, {port: "valve", env: "relay", index: 2}], ctx()))
            .toEqual([undefined, undefined]);
    });
});

describe("bindings files", () => {
    it("reads the engine's form and writes it back", () => {
        const text = bindingsFileText("sim", [{graph: "p", port: "level", env: "ain", index: 1},
            {graph: "p", port: "valve", env: "temp"}]);
        expect(JSON.parse(text)).toEqual({environment: "sim", bindings: [{port: "level", env: "ain", index: 1},
            {port: "valve", env: "temp"}]});
        expect(parseBindingsFile(text)).toEqual({environment: "sim", bindings: [{port: "level", env: "ain", index: 1},
            {port: "valve", env: "temp"}]});
        expect(parseBindingsFile('{"environment":"e","bindings":[{"port":"p","env":"x","index":null}]}'))
            .toEqual({environment: "e", bindings: [{port: "p", env: "x"}]});
    });

    it("says what is wrong with a file that is not one", () => {
        expect(parseBindingsFile("{")).toBe("Not a bindings file: not JSON");
        expect(parseBindingsFile("[]")).toBe("Not a bindings file: expected an object");
        expect(parseBindingsFile('{"bindings":[]}')).toBe("environment: expected a non-empty string");
        expect(parseBindingsFile('{"environment":"e","bindings":{}}')).toBe("bindings: expected an array");
        expect(parseBindingsFile('{"environment":"e","bindings":[{"port":"p"}]}'))
            .toBe("bindings[0]: env: expected a non-empty string");
        expect(parseBindingsFile('{"environment":"e","bindings":[{"port":"p","env":"x","index":0}]}'))
            .toBe("bindings[0]: index: expected an integer of at least 1");
        // The earlier form bound block ports: not read (plan task 70).
        expect(parseBindingsFile('{"environment":"e","bindings":[{"block":"a","port":"p","env":"x"}]}'))
            .toBe("bindings[0]: block: block ports are not bound; bind a process port");
    });
});

describe("the mapping dialog's rows", () => {
    it("keeps what exists, leaves the rest to choose, and checks the choices", () => {
        const c = ctx();
        const rows = initialRows([{port: "level", env: "ain", index: 3}, {port: "gone", env: "temp"},
            {port: "valve", env: "relay", index: 1}], c);
        expect(rows.map(r => [r.port, r.channel])).toEqual([["level", ""], ["", "temp"], ["valve", "relay[1]"]]);
        expect(rowProblems(rows, c)).toEqual(["Choose a channel", "Choose a process port", undefined]);
        rows[0].channel = "ain[2]";
        rows[1].port = "level";
        expect(rowProblems(rows, c)).toEqual([undefined, "Input already connected: level", undefined]);
        rows[1].skip = true;
        expect(rowProblems(rows, c)).toEqual([undefined, undefined, undefined]);
        expect(rowEntry(rows[0])).toEqual({port: "level", env: "ain", index: 2});
    });
});
