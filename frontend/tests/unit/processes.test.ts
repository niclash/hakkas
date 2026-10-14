import {describe, expect, it} from "vitest";
import {webcrypto} from "node:crypto";
import {formatUptime, isSaveFailure, newProcessId, uptimeNow} from "@/components/processes";

// 06 §4.6: the process list's uptime, a failed save, and new processes' ids.

describe("processes", () => {
    it("uptime for people", () => {
        expect(formatUptime(0)).toBe("less than a minute");
        expect(formatUptime(59)).toBe("less than a minute");
        expect(formatUptime(60)).toBe("1 min");
        expect(formatUptime(3599)).toBe("59 min");
        expect(formatUptime(3600)).toBe("1 h");
        expect(formatUptime(3600 + 5 * 60)).toBe("1 h 5 min");
        expect(formatUptime(86400)).toBe("1 d");
        expect(formatUptime(86400 * 3 + 3600 * 4 + 59)).toBe("3 d 4 h");
    });

    it("the uptime counts on while the process runs", () => {
        expect(uptimeNow(10, 1000, true, 31_500)).toBe(40);
        expect(uptimeNow(10, 1000, false, 31_500)).toBe(10);
        expect(uptimeNow(10, 5000, true, 1000)).toBe(10);
    });

    it("a failed save of the process", () => {
        expect(isSaveFailure("Unable to save graph: A (x) (g1)", "g1")).toBe(true);
        expect(isSaveFailure("Unable to save graph: A (g12)", "g1")).toBe(false);
        expect(isSaveFailure("Unknown graph: g1", "g1")).toBe(false);
    });

    it("a new process's id is a lowercase UUID v4", () => {
        expect(newProcessId(b => b.fill(0xff))).toBe("ffffffff-ffff-4fff-bfff-ffffffffffff");
        expect(newProcessId(b => b.fill(0))).toBe("00000000-0000-4000-8000-000000000000");
        expect(newProcessId(b => webcrypto.getRandomValues(b))).toMatch(/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
    });
});
