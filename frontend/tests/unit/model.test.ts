import {beforeEach, describe, expect, it} from "vitest";
import {attachModel, componentTemplates, messageLog, vueModel} from "@/components/model";
import {describeEnvironment} from "@/components/environment";
import {testConnection, TestConnection} from "./harness";

// The client model changes only from events (02 §1.1).
describe("model", () => {
    let t: TestConnection;
    const component = (name: string, description = "") => ({
        name, description, subgraph: false, icon: "",
        inPorts: [{id: "in1", description: "", type: "number", addressable: false}],
        outPorts: [{id: "out", description: "", type: "number", addressable: false}],
    });
    const status = (graph: string, name: string, running = false) => ({
        graph, name, description: "d", uptime: 0, running, started: running, debug: false,
    });

    beforeEach(() => {
        t = testConnection();
        attachModel(t.conn);
    });

    it("collects the block types, replacing a type of the same name", () => {
        t.receive("component", "component", component("math/Add2"));
        t.receive("component", "component", component("math/Mult2"));
        t.receive("component", "component", component("math/Add2", "changed"));
        t.receive("component", "componentsready", 2);
        expect(vueModel.components.value.map(c => c.name)).toEqual(["math/Add2", "math/Mult2"]);
        expect(componentTemplates["math/Add2"].description).toBe("changed");
    });

    it("starts empty for every connection", () => {
        t.receive("component", "component", component("math/Add2"));
        t.receive("network", "status", status("g1", "One"));
        attachModel(testConnection().conn);
        expect(vueModel.components.value).toEqual([]);
        expect(vueModel.graphs.value.size).toBe(0);
        expect(componentTemplates).toEqual({});
    });

    it("tracks the processes from status, new_graph, delete_graph, started, stopped and rename", () => {
        t.receive("network", "status", status("g1", "One"));
        t.receive("runtime", "new_graph", {graph: "g2", name: "Two", description: "", icon: ""});
        expect([...vueModel.graphs.value.keys()]).toEqual(["g1", "g2"]);
        expect(vueModel.graphs.value.get("g2")!.name).toBe("Two");

        t.receive("network", "started", {graph: "g1", time: "2026-01-01T00:00:00Z", started: true, running: true, debug: false});
        expect(vueModel.graphs.value.get("g1")!.running).toBe(true);
        t.receive("network", "stopped", {graph: "g1", time: "2026-01-01T00:00:00Z", uptime: 3, started: false, running: false, debug: false});
        expect(vueModel.graphs.value.get("g1")!.running).toBe(false);

        t.receive("graph", "rename", {id: "g1", from: "One", to: "Uno"});
        expect(vueModel.graphs.value.get("g1")!.name).toBe("Uno");
        t.receive("runtime", "delete_graph", {graph: "g2", name: "Two"});
        expect([...vueModel.graphs.value.keys()]).toEqual(["g1"]);
    });

    it("takes the open process from graph:clear, not from a request", () => {
        t.receive("network", "status", status("g1", "One"));
        t.conn.proto.graph.request_connect("g1");
        expect(vueModel.currentGraphId.value).toBe("");
        expect(t.sent[0]).toMatchObject({protocol: "graph", command: "connect", payload: {id: "g1"}});
        t.receive("graph", "clear", {id: "g1", name: "One", main: true, description: "", icon: ""});
        expect(vueModel.currentGraphId.value).toBe("g1");
        expect(t.conn.currentGraph).toBe("g1");
    });

    it("an engine without an environment is no error, nor a message (06 §4.14)", () => {
        messageLog.clear();
        vueModel.errors.value = [];
        describeEnvironment(t.conn);
        t.receive("network", "error", {stack: "", message: "No environment"});
        expect(vueModel.errors.value).toEqual([]);
        expect(messageLog.items).toEqual([]);
        // Not as the reply to environment:describe: a refusal like any other.
        t.receive("network", "error", {stack: "", message: "No environment"});
        expect(vueModel.errors.value).toEqual(["No environment"]);
        expect(messageLog.items.map(m => m.refused)).toEqual([true]);
    });

    it("lists errors, the latest five", () => {
        for (let i = 1; i <= 7; i++)
            t.receive("network", "error", {stack: "", message: "Error " + i});
        expect(vueModel.errors.value).toEqual(["Error 3", "Error 4", "Error 5", "Error 6", "Error 7"]);
    });

    it("names a new block after its type, with the first free number", () => {
        const taken = new Set(["Add2_1", "Add2_3"]);
        expect(vueModel.nextName("math/Add2", n => taken.has(n))).toBe("Add2_2");
        expect(vueModel.nextName("Plain", () => false)).toBe("Plain_1");
    });

    it("never changes on a request", () => {
        t.conn.proto.runtime.request_new_graph("New", "", "");
        t.conn.proto.graph.request_addnode("Add2_1", "math/Add2", 1, 2);
        expect(vueModel.graphs.value.size).toBe(0);
        expect(t.sent.map(p => p.command)).toEqual(["new_graph", "addnode"]);
    });
});
