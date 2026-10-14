import {describe, expect, it} from "vitest";
import {checkPorts, checkTypeName, componentCommands, defaultPorts, SelBlock, suggestName} from "@/components/composite";

// 06 §4.9: the port mapping of a new composite type, and the commands that build it (02 §3.4.3).

const add2 = (id: string, x = 0, initials: Record<string, string> = {}): SelBlock => ({
    id, component: "math/Add2", x, y: 10, initials,
    inputs: [{port: "in1", type: "number"}, {port: "in2", type: "number"}],
    outputs: [{port: "out", type: "number"}],
});
const link = (s: string, t: string) => {
    const [sn, sp] = s.split("."), [tn, tp] = t.split(".");
    return {src: {node: sn, port: sp}, tgt: {node: tn, port: tp}};
};

describe("defaultPorts", () => {
    const blocks = [add2("a"), add2("b", 200, {in2: "5"})];
    const links = [link("x.out", "a.in1"), link("a.out", "b.in1"), link("b.out", "y.in1"), link("x.out", "b.in1")];

    it("crossing connections are exposed, named after their ports, clashes after their blocks", () => {
        const rows = defaultPorts(blocks, links, []);
        expect(rows.filter(r => r.exposed).map(r => [r.direction, r.node + "." + r.port, r.name, r.why])).toEqual([
            ["in", "a.in1", "a_in1", "connection"],
            ["in", "b.in1", "b_in1", "connection"],
            ["out", "b.out", "out", "connection"],
        ]);
        // Every other port is listed too, with a free name.
        expect(rows.filter(r => !r.exposed).map(r => r.node + "." + r.port + "=" + r.name))
            .toEqual(["a.in2=in2", "b.in2=b_in2", "a.out=a_out"]);
        expect(rows.find(r => r.node === "b" && r.port === "in2")!.fixed).toBe("5");
    });

    it("process ports are exposed under their public names", () => {
        const rows = defaultPorts(blocks, links, [{direction: "in", name: "flow", node: "a", port: "in2"},
            {direction: "out", name: "total", node: "a", port: "out"}]);
        expect(rows.filter(r => r.exposed).map(r => r.node + "." + r.port + "=" + r.name + ":" + r.why)).toEqual([
            "a.in1=a_in1:connection", "a.in2=flow:process-port", "b.in1=b_in1:connection",
            "a.out=total:process-port", "b.out=out:connection"]);
    });
});

describe("names", () => {
    it("suggestions follow 03 §5.2's default names", () => {
        expect(suggestName(new Set(), "a", "in1")).toBe("in1");
        expect(suggestName(new Set(["in1"]), "a", "in1")).toBe("a_in1");
        expect(suggestName(new Set(["in1", "a_in1", "in1_2"]), "a", "in1")).toBe("in1_3");
        expect(suggestName(new Set(), "a", "x.y")).toBe("port_2");
    });

    it("the type's name and the ports' names are checked", () => {
        expect(checkTypeName("user/Pump", ["math/Add2"])).toBe("");
        expect(checkTypeName("math/Add2", ["math/Add2"])).toBe("A block type with this name exists");
        expect(checkTypeName(" ", [])).not.toBe("");
        const rows = defaultPorts([add2("a")], [], []).map(r => ({...r, exposed: true}));
        rows[1].name = "in1";
        rows[2].name = "bad.name";
        expect([...checkPorts(rows).entries()].sort()).toEqual([[0, "Used twice"], [1, "Used twice"], [2, "1 to 64 characters, no “.”"]]);
    });
});

describe("componentCommands", () => {
    it("the component graph: its blocks, inner connections, fixed values but on exposed inputs, ports, persist", () => {
        const blocks = [add2("a", 0, {in2: "1"}), add2("b", 200, {in2: "5"})];
        const links = [link("x.out", "a.in1"), link("a.out", "b.in1")];
        const rows = defaultPorts(blocks, links, []);
        rows.find(r => r.node === "b" && r.port === "in2")!.exposed = true;
        const cmds = componentCommands("user/Pair", "Pair", "Two adders", blocks, links, rows);
        expect(cmds.map(c => c.command)).toEqual(["clear", "addnode", "addnode", "addedge", "addinitial",
            "addinport", "addinport", "persist"]);
        expect(cmds[0].payload).toEqual({id: "user/Pair", name: "Pair", library: "user", main: false,
            description: "Two adders", icon: ""});
        expect(cmds[1].payload).toEqual({graph: "user/Pair", id: "a", component: "math/Add2", metadata: {x: 0, y: 10}});
        expect(cmds[3].payload).toEqual({graph: "user/Pair", src: {node: "a", port: "out"}, tgt: {node: "b", port: "in1"}});
        expect(cmds[4].payload).toEqual({graph: "user/Pair", tgt: {node: "a", port: "in2"}, src: {data: "1"}});
        expect(cmds[5].payload).toEqual({graph: "user/Pair", node: "a", port: "in1", public: "in1"});
        expect(cmds[6].payload).toEqual({graph: "user/Pair", node: "b", port: "in2", public: "b_in2"});
        expect(cmds[7]).toEqual({protocol: "network", command: "persist", payload: {graph: "user/Pair"}});
    });
});
