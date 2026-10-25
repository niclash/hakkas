import {describe, expect, it} from "vitest";
import {Cmd, factsOf, History, holds, inverseOf, Link, makeStep, push, StateReader} from "@/components/undo";

// 06 §4.13: undo steps — the inverse of a user action restores the process; a step is checked
// against the process before it is undone.

// A process the commands apply to, as the engine would.
class Fake implements StateReader {
    blocks = new Map<string, { component: string, x: number, y: number }>();
    links: Link[] = [];
    inits = new Map<string, string>();
    gports = new Map<string, { node: string, port: string }>();   // "in:name" / "out:name"
    // Environment bindings of process ports (04 §4): "port|channel"; the channel says the
    // direction (ain and temp are sensors, relay actuators).
    binds = new Map<string, { port: string, dir: "in" | "out", env: string, index?: number }>();

    block(id: string) { return this.blocks.get(id); }
    initial(node: string, port: string) { return this.inits.get(node + "." + port); }
    initials(node: string) {
        const r: Record<string, string> = {};
        for (const [k, v] of this.inits)
            if (k.startsWith(node + ".")) r[k.substring(node.length + 1)] = v;
        return r;
    }
    edges(node: string) { return this.links.filter(l => l.src.node === node || l.tgt.node === node); }
    edgeInto(node: string, port: string) { return this.links.find(l => l.tgt.node === node && l.tgt.port === port); }
    ports(node: string) {
        return [...this.gports].filter(([, v]) => v.node === node)
            .map(([k, v]) => ({direction: k.split(":")[0] as "in" | "out", name: k.substring(k.indexOf(":") + 1), port: v.port}));
    }
    port(direction: "in" | "out", name: string) { return this.gports.get(direction + ":" + name); }
    portBindings(direction: "in" | "out", name: string) {
        return [...this.binds.values()].filter(b => b.port === name && b.dir === direction)
            .map(b => b.index ? {env: b.env, index: b.index} : {env: b.env});
    }
    bound(port: string, channel: string) { return this.binds.has(port + "|" + channel); }
    private unbindPort(dir: "in" | "out", name: string) {
        for (const [k, b] of [...this.binds]) if (b.port === name && b.dir === dir) this.binds.delete(k);
    }

    apply(c: Cmd) {
        const p = c.payload;
        const dir = c.command.includes("inport") ? "in" : "out";
        switch (c.command) {
            case "addnode": this.blocks.set(p.id, {component: p.component, x: p.metadata?.x ?? 0, y: p.metadata?.y ?? 0}); break;
            case "removenode":
                this.links = this.links.filter(l => l.src.node !== p.id && l.tgt.node !== p.id);
                for (const k of [...this.inits.keys()]) if (k.startsWith(p.id + ".")) this.inits.delete(k);
                for (const [k, v] of [...this.gports])
                    if (v.node === p.id) {
                        this.unbindPort(k.split(":")[0] as "in" | "out", k.substring(k.indexOf(":") + 1));
                        this.gports.delete(k);
                    }
                this.blocks.delete(p.id);
                break;
            case "renamenode": {
                const b = this.blocks.get(p.from)!;
                this.blocks.delete(p.from);
                this.blocks.set(p.to, b);
                for (const v of this.gports.values()) if (v.node === p.from) v.node = p.to;
                break;
            }
            case "changenode": Object.assign(this.blocks.get(p.id)!, {x: p.metadata.x, y: p.metadata.y}); break;
            case "addedge":
                expect(this.blocks.has(p.src.node) && this.blocks.has(p.tgt.node)).toBe(true);
                this.links.push({src: {...p.src}, tgt: {...p.tgt}});
                break;
            case "removeedge":
                this.links = this.links.filter(l => JSON.stringify(l) !== JSON.stringify({src: p.src, tgt: p.tgt}));
                break;
            case "addinitial": this.inits.set(p.tgt.node + "." + p.tgt.port, String(p.src.data)); break;
            case "removeinitial": this.inits.delete(p.tgt.node + "." + p.tgt.port); break;
            case "addinport": case "addoutport": this.gports.set(dir + ":" + p.public, {node: p.node, port: p.port}); break;
            case "removeinport": case "removeoutport":
                this.unbindPort(dir, p.public);   // its bindings go with it (04 §4)
                this.gports.delete(dir + ":" + p.public);
                break;
            case "bind": {
                const bdir = p.env === "relay" ? "out" : "in";
                expect(this.gports.has(bdir + ":" + p.port)).toBe(true);
                const channel = p.index ? `${p.env}[${p.index}]` : p.env;
                this.binds.set(p.port + "|" + channel, {port: p.port, dir: bdir, env: p.env, ...(p.index ? {index: p.index} : {})});
                break;
            }
            case "unbind": this.binds.delete(p.port + "|" + (p.index ? `${p.env}[${p.index}]` : p.env)); break;
            case "renameinport": case "renameoutport": {
                const v = this.gports.get(dir + ":" + p.from)!;
                this.gports.delete(dir + ":" + p.from);
                this.gports.set(dir + ":" + p.to, v);
                // Bindings follow the rename (04 §4).
                for (const [k, b] of [...this.binds])
                    if (b.port === p.from && b.dir === dir) {
                        this.binds.delete(k);
                        this.binds.set(p.to + k.substring(p.from.length), {...b, port: p.to});
                    }
                break;
            }
        }
    }

    snapshot() {
        return JSON.stringify({b: [...this.blocks].sort(), l: this.links.map(l => JSON.stringify(l)).sort(),
            i: [...this.inits].sort(), p: [...this.gports].sort(), e: [...this.binds].map(x => JSON.stringify(x)).sort()});
    }
}

const g = "g";
const cmd = (command: string, payload: Record<string, any>): Cmd => ({command, payload: {graph: g, ...payload}});
const edge = (s: string, t: string) => {
    const [sn, sp] = s.split("."), [tn, tp] = t.split(".");
    return {src: {node: sn, port: sp}, tgt: {node: tn, port: tp}};
};

function process(): Fake {
    const f = new Fake();
    f.apply(cmd("addnode", {id: "a", component: "math/Add2", metadata: {x: 10, y: 20}}));
    f.apply(cmd("addnode", {id: "b", component: "math/Add2", metadata: {x: 200, y: 20}}));
    f.apply(cmd("addnode", {id: "c", component: "math/Add2", metadata: {x: 400, y: 20}}));
    f.apply(cmd("addedge", edge("a.out", "b.in1")));
    f.apply(cmd("addedge", edge("b.out", "c.in1")));
    f.apply(cmd("addinitial", {tgt: {node: "a", port: "in2"}, src: {data: "5"}}));
    f.apply(cmd("addinport", {node: "a", port: "in1", public: "flow"}));
    f.apply(cmd("addoutport", {node: "b", port: "out", public: "valve"}));
    f.apply(cmd("addoutport", {node: "c", port: "out", public: "lamp"}));
    f.apply(cmd("addinport", {node: "c", port: "in2", public: "level"}));
    f.apply(cmd("bind", {port: "valve", env: "relay", index: 2}));
    return f;
}

// The user action's commands: the step is made in the state before them, applied, checked and
// undone; the process must be as it was, and the redo then as after.
function roundTrip(cmds: Cmd[]) {
    const f = process();
    const before = f.snapshot();
    const step = makeStep(g, cmds, cmds.map(c => inverseOf(c, f)));
    cmds.forEach(c => f.apply(c));
    const after = f.snapshot();
    expect(holds(factsOf(step.forward), f)).toBe(true);
    step.inverse.forEach(c => f.apply(c));
    expect(f.snapshot()).toBe(before);
    expect(holds(factsOf(step.inverse), f)).toBe(true);
    step.forward.forEach(c => f.apply(c));
    expect(f.snapshot()).toBe(after);
    return step;
}

describe("undo steps", () => {
    it("deleting blocks connected to each other brings back blocks, edges, values and ports", () => {
        const step = roundTrip([cmd("removenode", {id: "a"}), cmd("removenode", {id: "b"})]);
        expect(step.inverse.map(c => c.command)).toEqual(["addnode", "addnode", "addedge", "addedge", "addinitial",
            "addoutport", "addinport", "bind"]);
    });

    // 06 §4.14: bindings (of process ports, plan task 70) are undone like the rest; a bindings
    // file applied is one step.
    it("bindings: bound, unbound, a file replacing them", () => {
        roundTrip([cmd("bind", {port: "level", env: "temp"})]);
        roundTrip([cmd("unbind", {port: "valve", env: "relay", index: 2})]);
        const file = roundTrip([cmd("unbind", {port: "valve", env: "relay", index: 2}),
            cmd("bind", {port: "lamp", env: "relay", index: 2}), cmd("bind", {port: "level", env: "ain", index: 1})]);
        expect(file.inverse.map(c => c.command)).toEqual(["unbind", "unbind", "bind"]);
        const f = process();
        const cmds = [cmd("bind", {port: "level", env: "temp"})];
        const step = makeStep(g, cmds, cmds.map(c => inverseOf(c, f)));
        cmds.forEach(c => f.apply(c));
        f.apply(cmd("unbind", {port: "level", env: "temp"}));   // another user
        expect(holds(factsOf(step.forward), f)).toBe(false);
    });

    it("bindings go and come back with their process port, and follow its rename", () => {
        const removed = roundTrip([cmd("removeoutport", {public: "valve"})]);
        expect(removed.inverse.map(c => c.command)).toEqual(["addoutport", "bind"]);
        roundTrip([cmd("renameoutport", {from: "valve", to: "v2"})]);
        // A block port bound through a new process port: one step (06 §4.14).
        const shortcut = roundTrip([cmd("addinport", {node: "b", port: "in2", public: "pressure"}),
            cmd("bind", {port: "pressure", env: "ain", index: 2})]);
        expect(shortcut.inverse.map(c => c.command)).toEqual(["unbind", "removeinport"]);
    });

    it("adding, renaming, moving (a drag is one step), connecting", () => {
        roundTrip([cmd("addnode", {id: "d", component: "math/Add2", metadata: {x: 0, y: 0}}),
            cmd("addedge", edge("d.out", "c.in2"))]);
        roundTrip([cmd("renamenode", {from: "b", to: "bee"})]);
        const drag = roundTrip([cmd("changenode", {id: "a", metadata: {x: 20, y: 30}}),
            cmd("changenode", {id: "a", metadata: {x: 60, y: 90}})]);
        expect(drag.forward).toHaveLength(1);
        expect(drag.inverse).toEqual([cmd("changenode", {id: "a", metadata: {x: 10, y: 20}})]);
    });

    it("values: set, changed, cleared; a connection replacing a value; ports", () => {
        roundTrip([cmd("addinitial", {tgt: {node: "c", port: "in2"}, src: {data: 3}})]);
        roundTrip([cmd("addinitial", {tgt: {node: "a", port: "in2"}, src: {data: 7}})]);
        roundTrip([cmd("removeinitial", {tgt: {node: "a", port: "in2"}})]);
        roundTrip([cmd("removeinitial", {tgt: {node: "a", port: "in2"}}), cmd("addedge", edge("c.out", "a.in2"))]);
        roundTrip([cmd("removeedge", edge("a.out", "b.in1"))]);
        roundTrip([cmd("renameinport", {from: "flow", to: "f"})]);
        roundTrip([cmd("removeinport", {public: "flow"}), cmd("addoutport", {node: "c", port: "out", public: "y"})]);
    });

    it("a step changed by someone else does not hold any more", () => {
        const f = process();
        const cmds = [cmd("changenode", {id: "a", metadata: {x: 99, y: 99}})];
        const step = makeStep(g, cmds, cmds.map(c => inverseOf(c, f)));
        cmds.forEach(c => f.apply(c));
        f.apply(cmd("changenode", {id: "a", metadata: {x: 5, y: 5}}));   // another user
        expect(holds(factsOf(step.forward), f)).toBe(false);
        f.apply(cmd("changenode", {id: "a", metadata: {x: 99.2, y: 98.8}}));
        expect(holds(factsOf(step.forward), f)).toBe(true);              // rounded to the pixel
    });

    it("the history keeps 500 steps and a new step clears what could be redone", () => {
        const h: History = {undo: [], redo: [{graph: g, forward: [], inverse: []}]};
        for (let i = 0; i < 502; i++)
            push(h, {graph: g, forward: [cmd("removenode", {id: "n" + i})], inverse: []});
        expect(h.undo).toHaveLength(500);
        expect(h.undo[0].forward[0].payload.id).toBe("n2");
        expect(h.redo).toEqual([]);
    });
});
