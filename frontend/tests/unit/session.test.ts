import {describe, expect, it} from "vitest";
import {forgetSession, keepSession, keptSession, sessionKey} from "@/session";

// 06 §2: what resumes a session, kept in the browser.

function memory() {
    const items = new Map<string, string>();
    return {
        items,
        getItem: (k: string) => items.get(k) ?? null,
        setItem: (k: string, v: string) => { items.set(k, v); },
        removeItem: (k: string) => { items.delete(k); },
    };
}

describe("kept session", () => {
    const key = new Uint8Array(32).map((_, i) => i);

    it("keeps, reads back and forgets the user, id and key", () => {
        const s = memory();
        expect(keptSession(s)).toBeUndefined();
        keepSession({user: "alice", id: "aWQx", key}, s);
        const kept = keptSession(s);
        expect(kept?.user).toBe("alice");
        expect(kept?.id).toBe("aWQx");
        expect(Array.from(kept!.key)).toEqual(Array.from(key));
        forgetSession(s);
        expect(s.items.size).toBe(0);
    });

    it("ignores anything else stored, and storage that fails", () => {
        const s = memory();
        for (const text of ["", "null", "{}", "[1]", "{\"user\":\"a\",\"id\":\"b\",\"key\":\"AAAA\"}",
                            "{\"user\":\"a\",\"id\":1,\"key\":\"\"}", "{\"user\":\"a\",\"id\":\"b\",\"key\":\"%%\"}"]) {
            s.items.set(sessionKey, text);
            expect(keptSession(s)).toBeUndefined();
        }
        const failing = {
            getItem: () => { throw new Error("denied"); },
            setItem: () => { throw new Error("denied"); },
            removeItem: () => { throw new Error("denied"); },
        };
        expect(keptSession(failing)).toBeUndefined();
        keepSession({user: "alice", id: "aWQx", key}, failing);
        forgetSession(failing);
        // No localStorage at all (Node): nothing kept, nothing thrown.
        expect(keptSession()).toBeUndefined();
    });
});
