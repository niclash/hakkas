import {describe, expect, it} from "vitest";
import {PendingRename} from "@/components/renaming";

// 06 §4.2: a rename's refusal is shown at the name field; other errors are not taken.

describe("PendingRename", () => {
    it("takes only the refusals the pending rename can cause", () => {
        const p = new PendingRename();
        expect(p.refusal("Node exists: b")).toBe(false);
        p.sent("a", "b");
        expect(p.refusal("Node exists: c")).toBe(false);
        expect(p.refusal("Unknown node: x")).toBe(false);
        expect(p.refusal("Node exists: b")).toBe(true);
        expect(p.active).toBe(false);
        expect(p.refusal("Node exists: b")).toBe(false);

        for (const message of ["Invalid id", "Unknown node: a", "Not permitted: graph:renamenode"]) {
            p.sent("a", "b");
            expect(p.refusal(message)).toBe(true);
        }
    });

    it("is done when the block shows the new name, moot when it shows another", () => {
        const p = new PendingRename();
        p.sent("a", "b");
        p.shown("a");
        expect(p.active).toBe(true);
        p.shown("b");
        expect(p.active).toBe(false);

        p.sent("a", "b");
        p.shown("c");
        expect(p.active).toBe(false);
        p.sent("a", "b");
        p.shown(undefined);
        expect(p.active).toBe(false);
        p.sent("a", "b");
        p.cancel();
        expect(p.refusal("Invalid id")).toBe(false);
    });
});
