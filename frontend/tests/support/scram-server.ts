// The server's side of the login (specifications/02-wire-protocol.md §2.2–§2.3), written with
// node:crypto, independently of the frontend's @noble/hashes code, so that each checks the other.

import {createHash, createHmac, pbkdf2Sync, randomBytes, timingSafeEqual} from "node:crypto";

// A users.json record (02 §2.2).
export interface UserRecord {
    user: string;
    role: "admin" | "editor" | "operator" | "viewer";
    salt: string;
    iterations: number;
    stored_key: string;
    server_key: string;
}

const hmac = (key: Buffer, text: string) => createHmac("sha256", key).update(text, "utf8").digest();
const sha256 = (data: Buffer) => createHash("sha256").update(data).digest();

// Few iterations keep the tests fast; the engine accepts any count from 1 (02 §2.2).
export function makeUser(user: string, password: string, role: UserRecord["role"] = "editor",
                         iterations = 1024): UserRecord {
    const salt = randomBytes(16);
    const salted = pbkdf2Sync(Buffer.from(password, "utf8"), salt, iterations, 32, "sha256");
    return {
        user, role, salt: salt.toString("base64"), iterations,
        stored_key: sha256(hmac(salted, "Client Key")).toString("base64"),
        server_key: hmac(salted, "Server Key").toString("base64"),
    };
}

export function challengeReply(record: UserRecord, cnonce: string) {
    return {salt: record.salt, iterations: record.iterations, nonce: cnonce + randomBytes(16).toString("base64")};
}

function authMessage(record: UserRecord, nonce: string) {
    return record.user + "," + nonce + "," + record.salt + "," + record.iterations;
}

// Checks a login's proof like the engine does; returns the server signature, or undefined.
export function verifyProof(record: UserRecord, nonce: string, proof: string): string | undefined {
    const message = authMessage(record, nonce);
    const signature = hmac(Buffer.from(record.stored_key, "base64"), message);
    const p = Buffer.from(proof, "base64");
    if (p.length !== 32)
        return undefined;
    const clientKey = Buffer.alloc(32);
    for (let i = 0; i < 32; i++)
        clientKey[i] = p[i] ^ signature[i];
    if (!timingSafeEqual(sha256(clientKey), Buffer.from(record.stored_key, "base64")))
        return undefined;
    return hmac(Buffer.from(record.server_key, "base64"), message).toString("base64");
}
