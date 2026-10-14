import {describe, expect, it} from "vitest";
import {VALUE_GAP, valuePosition} from "@/components/floweditor/value-position";

// 06 §4.3: a value node is drawn left of its block, level with its input.

describe("valuePosition", () => {
    it("left of the block by the gap, its middle at the input's socket", () => {
        expect(valuePosition({x: 300, y: 100}, 58, {width: 110, height: 36}))
            .toEqual({x: 300 - 110 - VALUE_GAP, y: 100 + 58 - 18});
    });
});
