// What the browser keeps to resume a session without the password (06 §2, 02 §2.3.1): the
// user, the resume id and the resume key, in localStorage (key "hakkas.session"). Each origin
// is one engine, so there is one session to keep. Storage can be unavailable or hold anything,
// so every read is checked and every access may fail quietly.

import {fromBase64, toBase64} from "@/components/protocols/scram";

export interface KeptSession {
    user: string;
    id: string;
    key: Uint8Array;
}

export const sessionKey = "hakkas.session";

// The browser's localStorage; a parameter so tests pass their own.
type SessionStorage = Pick<Storage, "getItem" | "setItem" | "removeItem">;

export function keptSession(storage?: SessionStorage): KeptSession | undefined {
    try {
        const text = (storage ?? localStorage).getItem(sessionKey);
        if (text === null)
            return undefined;
        const v = JSON.parse(text);
        if (typeof v?.user !== "string" || typeof v?.id !== "string" || typeof v?.key !== "string")
            return undefined;
        const key = fromBase64(v.key);
        return key.length === 32 ? {user: v.user, id: v.id, key} : undefined;
    } catch {
        return undefined;
    }
}

export function keepSession(session: KeptSession, storage?: SessionStorage) {
    try {
        (storage ?? localStorage).setItem(sessionKey, JSON.stringify({user: session.user, id: session.id, key: toBase64(session.key)}));
    } catch {
        // No storage: a reload asks for the password.
    }
}

export function forgetSession(storage?: SessionStorage) {
    try {
        (storage ?? localStorage).removeItem(sessionKey);
    } catch {
        // Nothing kept.
    }
}

// The wait before reconnecting attempt n (1, 2, …) after a lost connection (06 §2): 1, 2, 4, 8
// and 16 seconds, then every 30 seconds.
export function reconnectDelay(attempt: number): number {
    return Math.min(1000 * 2 ** Math.max(0, attempt - 1), 30000);
}
