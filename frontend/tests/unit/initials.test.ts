import {describe, expect, it} from "vitest";
import {initialRefusals, parseTyped, restingValue, sameValue, valueEditor} from "@/components/initials";

// 06 §4.2: fixed values on inputs, edited to fit the port's type (02 §3.4).

describe("fixed values", () => {
    it("the editor fits the port's type; other types are numbers, as the engine reads them", () => {
        expect(valueEditor("bool")).toBe("bool");
        expect(valueEditor("string")).toBe("string");
        expect(valueEditor("number")).toBe("number");
        expect(valueEditor("int")).toBe("number");
        expect(valueEditor("")).toBe("number");
    });

    it("an input fed by nothing rests at its type's resting value", () => {
        expect(["number", "bool", "string", "other"].map(restingValue)).toEqual(["0", "false", "", "0"]);
    });

    it("a number field sends a JSON number, clears when empty, and refuses other text", () => {
        expect(parseTyped("number", " 4.5 ")).toEqual({value: 4.5});
        expect(parseTyped("number", "-1e3")).toEqual({value: -1000});
        expect(parseTyped("number", "0")).toEqual({value: 0});
        expect(parseTyped("number", "  ")).toEqual({clear: true});
        expect(parseTyped("number", "4.2 V")).toEqual({error: "Enter a number"});
        expect(parseTyped("number", "Infinity")).toEqual({error: "Enter a number"});
        expect(parseTyped("number", "NaN")).toEqual({error: "Enter a number"});
    });

    it("a text field sends what was typed, also nothing", () => {
        expect(parseTyped("string", " a b ")).toEqual({value: " a b "});
        expect(parseTyped("string", "")).toEqual({value: ""});
    });

    it("an unchanged value is recognized in the event's form", () => {
        expect(sameValue("number", "12", 12)).toBe(true);
        expect(sameValue("number", "12.5", 12.50)).toBe(true);
        expect(sameValue("number", "12", 13)).toBe(false);
        expect(sameValue("number", "", 0)).toBe(false);
        expect(sameValue("string", "a", "a")).toBe(true);
        expect(sameValue("string", "a", "a ")).toBe(false);
    });

    it("the refusals a command on an input can cause", () => {
        expect(initialRefusals("b", "in1", "addinitial")).toEqual([
            "Input already connected: b.in1", "Unknown node: b", "Unknown port: b.in1", "Not permitted: graph:addinitial"]);
        expect(initialRefusals("b", "in1", "removeinitial")).toEqual([
            "No such connection", "Unknown node: b", "Unknown port: b.in1", "Not permitted: graph:removeinitial"]);
    });
});
