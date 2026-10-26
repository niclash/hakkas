import {describe, expect, it} from "vitest";
import {
    DeviceState, fileName, GraphFile, graphFromSnapshot, insideCommands, parseTransferFile, portsOf, sameInside,
    transferFile, transferText, TypeFile, typeOrder, typesUsed, uploadPlan
} from "@/components/transfer";

// 06 §4.16: the process file, a graph from its snapshot, the commands that make it again, and what
// an upload would do.

const snapshot = [
    {command: "clear", payload: {id: "p1", name: "Pump", main: true, description: "d", icon: "cog"}},
    {command: "addnode", payload: {graph: "p1", id: "a", component: "math/Add2", metadata: {x: 10, y: 20, color: "red"}}},
    {command: "addnode", payload: {graph: "p1", id: "b", component: "user/Twice", metadata: {x: 300, y: 20}}},
    {command: "addinport", payload: {graph: "p1", name: "level", node: "a", port: "in1"}},
    {command: "addoutport", payload: {graph: "p1", name: "out", node: "b", port: "out"}},
    {command: "addinitial", payload: {graph: "p1", src: {data: "5"}, tgt: {node: "a", port: "in2"}}},
    {command: "addedge", payload: {graph: "p1", src: {node: "a", port: "out"}, tgt: {node: "b", port: "in"}, metadata: {route: 3}}},
    {command: "addnode", payload: {graph: "other", id: "x", component: "math/Add2", metadata: {x: 0, y: 0}}},
];

const pump = graphFromSnapshot(snapshot) as GraphFile;

const twice: TypeFile = {
    id: "user/Twice", name: "Twice", library: "user", description: "", icon: "", started: false,
    blocks: {m: {component: "math/Multiply2", metadata: {x: 0, y: 0}}},
    connections: [{src: {data: "2"}, tgt: {block: "m", port: "in2"}}],
    inports: [{name: "in", block: "m", port: "in1"}], outports: [{name: "out", block: "m", port: "out"}],
};

describe("graphFromSnapshot", () => {
    it("is the graph file of the snapshot: blocks with their metadata, ports, initials, connections", () => {
        expect(pump).toEqual({
            id: "p1", name: "Pump", description: "d", icon: "cog", started: false,
            blocks: {a: {component: "math/Add2", metadata: {x: 10, y: 20, color: "red"}},
                b: {component: "user/Twice", metadata: {x: 300, y: 20}}},
            connections: [{src: {data: "5"}, tgt: {block: "a", port: "in2"}},
                {src: {block: "a", port: "out"}, tgt: {block: "b", port: "in"}, metadata: {route: 3}}],
            inports: [{name: "level", block: "a", port: "in1"}], outports: [{name: "out", block: "b", port: "out"}],
        });
        expect(graphFromSnapshot([])).toBeUndefined();
    });

    it("a component graph keeps its library", () => {
        const t = graphFromSnapshot([{command: "clear", payload: {id: "user/X", name: "X", main: false, library: "user"}}]);
        expect(t).toMatchObject({id: "user/X", library: "user"});
    });
});

describe("insideCommands", () => {
    it("makes the graph again, in the snapshot's order", () => {
        expect(insideCommands(pump, "p1").map(c => [c.command, c.payload])).toEqual([
            ["addnode", {graph: "p1", id: "a", component: "math/Add2", metadata: {x: 10, y: 20, color: "red"}}],
            ["addnode", {graph: "p1", id: "b", component: "user/Twice", metadata: {x: 300, y: 20}}],
            ["addinport", {graph: "p1", public: "level", node: "a", port: "in1"}],
            ["addoutport", {graph: "p1", public: "out", node: "b", port: "out"}],
            ["addinitial", {graph: "p1", src: {data: "5"}, tgt: {node: "a", port: "in2"}}],
            ["addedge", {graph: "p1", src: {node: "a", port: "out"}, tgt: {node: "b", port: "in"}, metadata: {route: 3}}],
        ]);
    });
});

describe("the file", () => {
    const file = transferFile([{graph: pump, bindings: {environment: "plant", bindings: [{port: "level", env: "ai", index: 2}]}}],
        [twice], {label: "Hakkas", id: "e1", version: "1"}, new Date("2026-10-26T10:00:00Z"));

    it("reads back what is written", () => {
        expect(file.saved).toBe("2026-10-26T10:00:00.000Z");
        expect(parseTransferFile(transferText(file))).toEqual(file);
    });

    it("takes a plain graph file, as the engine saves one, as one process", () => {
        const f = parseTransferFile(JSON.stringify({id: "g", name: "G", started: true,
            blocks: {a: {component: "math/Add2", metadata: {x: 1, y: 2}}},
            connections: [{src: {data: 4}, tgt: {process: "a", port: "in1"}}], inports: [], outports: []}));
        expect(typeof f).toBe("object");
        if (typeof f === "object") {
            expect(f.processes).toHaveLength(1);
            expect(f.processes[0].graph).toMatchObject({id: "g", started: true,
                connections: [{src: {data: 4}, tgt: {block: "a", port: "in1"}}]});
            expect(f.processes[0].bindings).toBeUndefined();
        }
    });

    it("says what is wrong", () => {
        expect(parseTransferFile("{")).toBe("Not a process file: not JSON");
        expect(parseTransferFile("[]")).toBe("Not a process file: expected an object");
        expect(parseTransferFile(`{"format":"x"}`)).toBe("Not a process file: format: expected hakkas/processes");
        expect(parseTransferFile(`{"format":"hakkas/processes","version":2}`)).toBe("version: expected 1");
        expect(parseTransferFile(`{"format":"hakkas/processes","version":1,"processes":[{"graph":{"id":""}}]}`))
            .toBe("processes[0].graph: id: expected a graph id");
        expect(parseTransferFile(`{"format":"hakkas/processes","version":1,"processes":[{"graph":{"id":"g",` +
            `"connections":[{"src":{"block":"a"},"tgt":{"block":"b","port":"x"}}]}}]}`))
            .toBe("processes[0].graph: connections[0]: expected {src, tgt}");
        expect(parseTransferFile(`{"format":"hakkas/processes","version":1,"processes":[{"graph":{"id":"g"},` +
            `"bindings":{"environment":"","bindings":[]}}]}`))
            .toBe("processes[0].bindings: environment: expected a non-empty string");
    });

    it("names files safely", () => {
        expect(fileName("Pump / tank 2", ".process.json")).toBe("Pump _ tank 2.process.json");
        expect(fileName("***", ".backup.json")).toBe("_.backup.json");
        expect(fileName("", ".x")).toBe("hakkas.x");
    });
});

describe("types", () => {
    const outer: TypeFile = {...twice, id: "user/Four", name: "Four",
        blocks: {t1: {component: "user/Twice", metadata: {x: 0, y: 0}}, t2: {component: "user/Twice", metadata: {x: 0, y: 0}}}};

    it("used by graphs, and the types those use", () => {
        const types = new Map([[twice.id, twice], [outer.id, outer]]);
        expect(typesUsed([pump], types)).toEqual(["user/Twice"]);
        const four: GraphFile = {...pump, blocks: {f: {component: "user/Four", metadata: {x: 0, y: 0}}}};
        expect(typesUsed([four], types).sort()).toEqual(["user/Four", "user/Twice"]);
    });

    it("are made after the types they use", () => {
        expect(typeOrder([outer, twice]).map(t => t.id)).toEqual(["user/Twice", "user/Four"]);
        const loop: TypeFile = {...twice, id: "user/Loop", blocks: {l: {component: "user/Loop", metadata: {x: 0, y: 0}}}};
        expect(typeOrder([loop]).map(t => t.id)).toEqual(["user/Loop"]);
    });

    it("are the same without regard to positions; an initial as text", () => {
        const moved: TypeFile = {...twice, blocks: {m: {component: "math/Multiply2", metadata: {x: 99, y: 99}}},
            connections: [{src: {data: 2}, tgt: {block: "m", port: "in2"}}]};
        expect(sameInside(twice, moved)).toBe(true);
        expect(sameInside(twice, {...twice, connections: []})).toBe(false);
        expect(sameInside(twice, {...twice, inports: [{name: "x", block: "m", port: "in1"}]})).toBe(false);
    });
});

describe("uploadPlan", () => {
    const device = (over: Partial<DeviceState> = {}): DeviceState => ({
        processes: new Map([["p1", {name: "Old pump", running: true}]]),
        types: new Map<string, TypeFile | null>([["math/Add2", null], ["math/Multiply2", null]]),
        environment: "plant", ...over,
    });

    it("replaces a process of the same id, adds a missing type, notes what the engine lacks", () => {
        const odd: GraphFile = {...pump, id: "p2", name: "Odd", blocks: {z: {component: "x/Zap", metadata: {x: 0, y: 0}}}};
        const file = transferFile([{graph: pump, bindings: {environment: "plant", bindings: [{port: "level", env: "ai"}]}},
            {graph: odd}], [twice], undefined);
        const plan = uploadPlan(file, device());
        expect(plan.types.map(t => [t.type.id, t.action])).toEqual([["user/Twice", "add"]]);
        expect(plan.processes.map(p => [p.entry.graph.id, p.replaces, p.missing, p.bindings, p.include])).toEqual([
            ["p1", {name: "Old pump", running: true}, [], "apply", true],
            ["p2", undefined, ["x/Zap"], "none", true],
        ]);
    });

    it("keeps the engine's type, the same or not, and a built-in one; bindings without an environment", () => {
        const file = transferFile([{graph: pump, bindings: {environment: "plant", bindings: [{port: "level", env: "ai"}]}}],
            [twice, {...twice, id: "math/Add2"}, {...twice, id: "user/Other"}], undefined);
        const plan = uploadPlan(file, device({
            environment: undefined,
            types: new Map<string, TypeFile | null>([["math/Add2", null], ["user/Twice", {...twice}],
                ["user/Other", {...twice, id: "user/Other", connections: []}]]),
        }));
        expect(plan.types.map(t => [t.type.id, t.action])).toEqual([
            ["user/Twice", "same"], ["math/Add2", "builtin"], ["user/Other", "different"]]);
        expect(plan.processes[0].bindings).toBe("no-environment");
    });
});

describe("portsOf", () => {
    it("is the process ports with their block ports, and the inputs with a source", () => {
        expect(portsOf(pump)).toEqual({inports: {level: "a.in1"}, outports: {out: "b.out"}, sourced: new Set(["a.in2", "b.in"])});
    });
});
