import {describe, expect, it} from "vitest";
import {addressFor, parseAddress, titleFor} from "@/address";

// 06 §2.1: the open process or type in the address.

describe("addresses", () => {
    it("a process, a type, nothing", () => {
        expect(addressFor({kind: "process", id: "0826152e-3014"})).toBe("/?process=0826152e-3014");
        expect(addressFor({kind: "type", id: "user/Pump"})).toBe("/?type=user%2FPump");
        expect(addressFor(undefined)).toBe("/");
    });

    it("read back, whatever the id holds", () => {
        for (const id of ["a.b", "user/Pump", "ä ö&=?#", "x"])
            expect(parseAddress(addressFor({kind: "process", id}).substring(1))).toEqual({kind: "process", id});
        expect(parseAddress("?type=user%2FPump")).toEqual({kind: "type", id: "user/Pump"});
        expect(parseAddress("")).toBeUndefined();
        expect(parseAddress("?process=")).toBeUndefined();
        expect(parseAddress("?other=1")).toBeUndefined();
    });

    it("the page's title", () => {
        expect(titleFor("Pumps")).toBe("Pumps — Hakkas");
        expect(titleFor(undefined)).toBe("Hakkas");
    });
});
