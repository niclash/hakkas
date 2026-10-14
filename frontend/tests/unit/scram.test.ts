import {describe, expect, it} from "vitest";
import {clientNonce, computeProof, computeResumeProof, fromBase64, toBase64} from "@/components/protocols/scram";
import {challengeReply, makeUser, resumeKey, verifyProof, verifyResumeProof} from "../support/scram-server";

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

    it("derives the server's resume key, and resume proofs it accepts (02 §2.3.1)", async () => {
        const record = makeUser("alice", "correct horse");
        const challenge = challengeReply(record, clientNonce());
        const result = await computeProof("alice", "correct horse", challenge);
        const key = resumeKey(record, challenge.nonce);
        expect(Buffer.from(result.resumeKey).equals(key)).toBe(true);

        const nonce = clientNonce() + "c2VydmVyIHBhcnQgb2YgaXQ=";
        const id = "AAECAwQFBgcICQoLDA0ODw";
        const resume = computeResumeProof("alice", nonce, id, result.resumeKey);
        expect(verifyResumeProof(key, "alice", nonce, id, resume.proof)).toBe(resume.serverSignature);
        // Bound to the user, the nonce and the id.
        expect(verifyResumeProof(key, "alicf", nonce, id, resume.proof)).toBeUndefined();
        expect(verifyResumeProof(key, "alice", nonce + "x", id, resume.proof)).toBeUndefined();
        expect(verifyResumeProof(key, "alice", nonce, id + "x", resume.proof)).toBeUndefined();
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
