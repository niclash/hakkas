// SCRAM-SHA-256 client side of the login handshake (specifications/02-wire-protocol.md §2.3).
//
// Pure JavaScript (@noble/hashes): crypto.subtle exists only in secure contexts (HTTPS or
// localhost), and the device is normally reached as plain http://<device-ip>.

import {pbkdf2Async} from "@noble/hashes/pbkdf2";
import {hmac} from "@noble/hashes/hmac";
import {sha256} from "@noble/hashes/sha256";
import {randomBytes, utf8ToBytes} from "@noble/hashes/utils";

export interface Challenge {
    salt: string;       // base64, as sent by the server
    iterations: number;
    nonce: string;      // client nonce + server nonce
}

export interface LoginProof {
    proof: string;              // base64, sent in environment:login
    serverSignature: string;    // base64, expected in the login reply
}

export interface PasswordProof extends LoginProof {
    // The key that resumes the session (02 §2.3.1); it is never sent.
    resumeKey: Uint8Array;
}

export function toBase64(bytes: Uint8Array): string {
    let s = "";
    for (const b of bytes)
        s += String.fromCharCode(b);
    return btoa(s);
}

export function fromBase64(text: string): Uint8Array {
    const s = atob(text);
    const bytes = new Uint8Array(s.length);
    for (let i = 0; i < s.length; i++)
        bytes[i] = s.charCodeAt(i);
    return bytes;
}

// The client nonce of environment:challenge: 18 random bytes (16–64 allowed), base64.
export function clientNonce(): string {
    return toBase64(randomBytes(18));
}

export async function computeProof(user: string, password: string, challenge: Challenge): Promise<PasswordProof> {
    const salted = await pbkdf2Async(sha256, utf8ToBytes(password), fromBase64(challenge.salt),
        {c: challenge.iterations, dkLen: 32});
    const clientKey = hmac(sha256, salted, utf8ToBytes("Client Key"));
    const storedKey = sha256(clientKey);
    const serverKey = hmac(sha256, salted, utf8ToBytes("Server Key"));
    // The salt is the base64 text of the challenge, the iterations its decimal form.
    const authMessage = utf8ToBytes(user + "," + challenge.nonce + "," + challenge.salt + "," + challenge.iterations);
    const clientSignature = hmac(sha256, storedKey, authMessage);
    const proof = new Uint8Array(32);
    for (let i = 0; i < 32; i++)
        proof[i] = clientKey[i] ^ clientSignature[i];
    const resumeKey = hmac(sha256, storedKey, utf8ToBytes("Resume Key," + challenge.nonce));
    salted.fill(0);
    clientKey.fill(0);
    return {
        proof: toBase64(proof),
        serverSignature: toBase64(hmac(sha256, serverKey, authMessage)),
        resumeKey,
    };
}

// A resume's proof (02 §2.3.1): no key stretching, the resume key signs user, nonce and id.
export function computeResumeProof(user: string, nonce: string, id: string, resumeKey: Uint8Array): LoginProof {
    const authMessage = user + "," + nonce + "," + id;
    return {
        proof: toBase64(hmac(sha256, resumeKey, utf8ToBytes(authMessage))),
        serverSignature: toBase64(hmac(sha256, resumeKey, utf8ToBytes("Server," + authMessage))),
    };
}
