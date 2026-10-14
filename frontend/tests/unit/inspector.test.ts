import {describe, expect, it} from "vitest";
import {inspect} from "@/components/inspector";
import {ComponentEvent} from "@/components/protocols/component";

// 06 §4.2: what the inspector shows for the selection, from the types' component:component.

const port = (id: string, type: string, description: string, def?: unknown) =>
    ({id, type, description, default: def, schema: "", required: false, addressable: false, values: []});

const types: Record<string, ComponentEvent> = {
    "math/Add2": {
        name: "math/Add2", fullId: "math/Add2", description: "Adds two numbers", icon: "plus", subgraph: false,
        inPorts: [port("in1", "number", "First term", 0), port("in2", "number", "", null)],
        outPorts: [port("out", "number", "The sum")],
    },
};

describe("inspect", () => {
    it("nothing selected", () => {
        expect(inspect([], types)).toEqual({kind: "none"});
    });

    it("one block: its id, type, description and ports in the type's order", () => {
        expect(inspect([{id: "Add2_1", component: "math/Add2"}], types)).toEqual({
            kind: "one", id: "Add2_1", component: "math/Add2", description: "Adds two numbers", known: true,
            inputs: [
                {id: "in1", type: "number", description: "First term", default: "0"},
                {id: "in2", type: "number", description: ""},
            ],
            outputs: [{id: "out", type: "number", description: "The sum"}],
        });
    });

    it("a block whose type is not registered", () => {
        expect(inspect([{id: "ghost", component: "nowhere/Missing"}], types)).toEqual({
            kind: "one", id: "ghost", component: "nowhere/Missing", description: "", known: false,
            inputs: [], outputs: [],
        });
    });

    it("several blocks: their ids, sorted", () => {
        expect(inspect([{id: "b", component: "math/Add2"}, {id: "a", component: "x/Y"}], types))
            .toEqual({kind: "many", ids: ["a", "b"]});
    });
});
