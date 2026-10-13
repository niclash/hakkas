import {describe, expect, it} from "vitest";
import {clientNonce, computeProof, fromBase64, toBase64} from "@/components/protocols/scram";
import {challengeReply, makeUser, verifyProof} from "../support/scram-server";

describe("scram", () => {
    it("computes a proof the server accepts, and the server's signature", async () => {
        const record = makeUser("alice", "correct horse", "editor", 4096);
        const challenge = challengeReply(record, clientNonce());
        const result = await computeProof("alice", "correct horse", challenge);
        expect(verifyProof(record, challenge.nonce, result.proof)).toBe(result.serverSignature);
    });

    it("gives a proof the server refuses for a wrong password", async () => {
        const record = makeUser("alice", "correct horse");
        const challenge = challengeReply(record, clientNonce());
        const result = await computeProof("alice", "wrong", challenge);
        expect(verifyProof(record, challenge.nonce, result.proof)).toBeUndefined();
    });

    it("uses the password's UTF-8 bytes and the user name as given", async () => {
        const record = makeUser("Åsa", "lösenord€");
        const challenge = challengeReply(record, clientNonce());
        const result = await computeProof("Åsa", "lösenord€", challenge);
        expect(verifyProof(record, challenge.nonce, result.proof)).toBe(result.serverSignature);
    });

    it("makes client nonces of 18 random bytes", () => {
        const a = clientNonce();
        expect(fromBase64(a)).toHaveLength(18);
        expect(clientNonce()).not.toBe(a);
    });

    it("round-trips base64", () => {
        const bytes = new Uint8Array([0, 1, 2, 250, 255, 128]);
        expect(toBase64(bytes)).toBe(Buffer.from(bytes).toString("base64"));
        expect(Array.from(fromBase64(toBase64(bytes)))).toEqual(Array.from(bytes));
    });
});
