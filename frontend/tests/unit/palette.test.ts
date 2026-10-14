import {describe, expect, it} from "vitest";
import {paletteGroups, socketKind, typeTooltip} from "@/components/palette";
import {ComponentEvent} from "@/components/protocols/component";

// 06 §4.5: the palette's families and search, its tooltips, and the sockets' classes.

const port = (id: string, type: string) =>
    ({id, type, description: "", schema: "", required: false, addressable: false, values: [], default: undefined});
const type = (name: string, description = "", inPorts = [port("in", "number")], outPorts = [port("out", "bool")]): ComponentEvent =>
    ({name, fullId: name, description, icon: "", subgraph: false, inPorts, outPorts});

const types = [type("math/Add2"), type("compare/Greater"), type("math/Mult2"), type("Loose")];

describe("paletteGroups", () => {
    it("by family, in arrival order, with the names after the family", () => {
        const groups = paletteGroups(types, "");
        expect(Object.keys(groups)).toEqual(["math", "compare", ""]);
        expect(groups.math.map(e => e.short)).toEqual(["Add2", "Mult2"]);
        expect(groups[""].map(e => e.name)).toEqual(["Loose"]);
    });

    it("only the matching types, and only families with a match", () => {
        const groups = paletteGroups(types, "eat");
        expect(Object.keys(groups)).toEqual(["compare"]);
        expect(paletteGroups(types, "MULT").math.map(e => e.name)).toEqual(["math/Mult2"]);
        expect(paletteGroups(types, "nothing")).toEqual({});
    });
});

describe("typeTooltip", () => {
    it("the name, the description and the ports with their types", () => {
        expect(typeTooltip(type("math/Add2", "out = in1+in2", [port("in1", "number"), port("in2", "")])))
            .toBe("math/Add2\nout = in1+in2\nInputs: in1 (number), in2\nOutputs: out (bool)");
        expect(typeTooltip(type("x/Y", "", [], []))).toBe("x/Y");
    });
});

describe("socketKind", () => {
    it("the engine's three types, else other", () => {
        expect(["number", "bool", "string", "int", ""].map(socketKind)).toEqual(["number", "bool", "string", "other", "other"]);
    });
});
