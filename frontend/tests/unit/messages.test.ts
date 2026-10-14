import {describe, expect, it} from "vitest";
import {errorText, MessageLog, parseLogLine, shown} from "@/components/messages";

// 06 §4.12: the messages panel.

describe("messages", () => {
    it("a mirrored log line, and what is none", () => {
        expect(parseLogLine("info auth.c:412: alice logged in from 10.0.0.2"))
            .toEqual({level: "info", source: "auth.c:412", text: "alice logged in from 10.0.0.2"});
        expect(parseLogLine("error graph_files.c:9: cannot write: a: b")?.text).toBe("cannot write: a: b");
        expect(parseLogLine("Unknown graph: x")).toBeUndefined();
        expect(parseLogLine("debug x.c:1: y")).toBeUndefined();
    });

    it("an error names its process when another is open", () => {
        expect(errorText("Unable to save graph: A (g)", "A", false)).toBe("A: Unable to save graph: A (g)");
        expect(errorText("Node exists: b", "A", true)).toBe("Node exists: b");
        expect(errorText("Login failed", undefined, false)).toBe("Login failed");
    });

    it("keeps the newest, and filters by level", () => {
        const log = new MessageLog(3);
        for (const level of ["fine", "info", "warn", "error"] as const)
            log.add({level, text: level});
        expect(log.items.map(m => m.text)).toEqual(["info", "warn", "error"]);
        expect(log.items.map(m => m.seq)).toEqual([2, 3, 4]);
        expect(shown(log.items, "warn").map(m => m.text)).toEqual(["warn", "error"]);
        log.clear();
        expect(log.items).toEqual([]);
    });
});
