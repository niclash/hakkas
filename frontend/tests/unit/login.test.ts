import {describe, expect, it} from "vitest";
import {challengeReply, makeUser, resumeKey, verifyProof, verifyResumeProof} from "../support/scram-server";
import {KeptSession} from "@/session";
import {testConnection, until} from "./harness";

// The handshake of 02 §2.3 against a fake server.
function startLogin(password: string) {
    const t = testConnection();
    const logins: unknown[] = [];
    const failures: string[] = [];
    t.conn.proto.environment.addListener("onLogin", (_c: unknown, login: unknown) => logins.push(login));
    t.conn.proto.environment.addListener("onLoginFailed", (_c: unknown, f: { message: string }) => failures.push(f.message));
    t.conn.proto.environment.request_login("alice", password);
    return {...t, logins, failures};
}

describe("login", () => {
    const record = makeUser("alice", "secret");

    it("sends a challenge, then a login with a valid proof, without a secret", async () => {
        const t = startLogin("secret");
        expect(t.sent).toHaveLength(1);
        const challenge = t.sent[0];
        expect(challenge).toMatchObject({protocol: "environment", command: "challenge", secret: ""});
        expect(challenge.payload.user).toBe("alice");

        const reply = challengeReply(record, challenge.payload.cnonce);
        t.receive("environment", "challenge", reply);
        await until(() => t.sent.length === 2);
        const login = t.sent[1];
        expect(login).toMatchObject({protocol: "environment", command: "login", secret: "",
                                     payload: {user: "alice", nonce: reply.nonce}});
        expect(verifyProof(record, reply.nonce, login.payload.proof)).toBeDefined();
    });

    it("accepts a login reply whose server signature matches", async () => {
        const t = startLogin("secret");
        const reply = challengeReply(record, t.sent[0].payload.cnonce);
        t.receive("environment", "challenge", reply);
        await until(() => t.sent.length === 2);
        const signature = verifyProof(record, reply.nonce, t.sent[1].payload.proof);
        t.receive("environment", "login", {secret: "s3cr3t", role: "editor", capabilities: ["protocol:graph"],
                                           server_signature: signature});
        expect(t.logins).toEqual([{secret: "s3cr3t", role: "editor", capabilities: ["protocol:graph"]}]);
        expect(t.failures).toEqual([]);
    });

    it("refuses a login reply whose server signature does not match", async () => {
        const t = startLogin("secret");
        const reply = challengeReply(record, t.sent[0].payload.cnonce);
        t.receive("environment", "challenge", reply);
        await until(() => t.sent.length === 2);
        t.receive("environment", "login", {secret: "s3cr3t", role: "editor", capabilities: [],
                                           server_signature: Buffer.alloc(32).toString("base64")});
        expect(t.logins).toEqual([]);
        expect(t.failures).toEqual(["The server could not prove its identity"]);
    });

    it("refuses a challenge whose nonce does not extend the client's", () => {
        const t = startLogin("secret");
        t.receive("environment", "challenge", challengeReply(record, "AAAAAAAAAAAAAAAAAAAAAAAA"));
        expect(t.failures).toEqual(["Unexpected login challenge"]);
        expect(t.sent).toHaveLength(1);
    });

    it("refuses a login reply without a challenge", () => {
        const t = startLogin("secret");
        t.conn.proto.environment.abandon_login();
        t.receive("environment", "login", {secret: "x", role: "editor", capabilities: [], server_signature: ""});
        expect(t.logins).toEqual([]);
    });

    it("gives what resumes the session when the reply has a resume id", async () => {
        const t = startLogin("secret");
        const reply = challengeReply(record, t.sent[0].payload.cnonce);
        t.receive("environment", "challenge", reply);
        await until(() => t.sent.length === 2);
        const signature = verifyProof(record, reply.nonce, t.sent[1].payload.proof);
        t.receive("environment", "login", {secret: "s", role: "editor", capabilities: [],
                                           server_signature: signature, resume: "aWQx"});
        const login = t.logins[0] as { resume: KeptSession };
        expect(login.resume.user).toBe("alice");
        expect(login.resume.id).toBe("aWQx");
        expect(Buffer.from(login.resume.key).equals(resumeKey(record, reply.nonce))).toBe(true);
    });

    it("reports a logout", () => {
        const t = testConnection();
        let logouts = 0;
        t.conn.proto.environment.addListener("onLogout", () => logouts++);
        t.receive("environment", "logout");   // no payload key at all (02 §1)
        expect(logouts).toBe(1);
    });
});

// Resuming a session (02 §2.3.1) against a fake server.
describe("resume", () => {
    const record = makeUser("alice", "secret");
    const key = resumeKey(record, "Y25vbmNlc2VydmVy");
    const kept: KeptSession = {user: "alice", id: "aWQx", key: new Uint8Array(key)};

    function startResume() {
        const t = testConnection();
        const logins: { resume?: KeptSession }[] = [];
        const failures: string[] = [];
        t.conn.proto.environment.addListener("onLogin", (_c: unknown, login: { resume?: KeptSession }) => logins.push(login));
        t.conn.proto.environment.addListener("onLoginFailed", (_c: unknown, f: { message: string }) => failures.push(f.message));
        t.conn.proto.environment.request_resume(kept);
        return {...t, logins, failures};
    }

    it("sends the id with the challenge, then a proof the server accepts, and keeps the new id", () => {
        const t = startResume();
        expect(t.sent[0]).toMatchObject({protocol: "environment", command: "challenge", secret: "",
                                         payload: {user: "alice", resume: "aWQx"}});
        const nonce = t.sent[0].payload.cnonce + "c2VydmVy";
        t.receive("environment", "challenge", {nonce});    // no salt, no iterations
        expect(t.sent).toHaveLength(2);
        expect(t.sent[1]).toMatchObject({protocol: "environment", command: "login", secret: "",
                                         payload: {user: "alice", nonce, resume: "aWQx"}});
        const signature = verifyResumeProof(key, "alice", nonce, "aWQx", t.sent[1].payload.proof);
        expect(signature).toBeDefined();
        t.receive("environment", "login", {secret: "s", role: "editor", capabilities: [],
                                           server_signature: signature, resume: "aWQy"});
        expect(t.failures).toEqual([]);
        expect(t.logins[0].resume?.id).toBe("aWQy");
        expect(Buffer.from(t.logins[0].resume!.key).equals(key)).toBe(true);
    });

    it("refuses a reply whose server signature does not match", () => {
        const t = startResume();
        t.receive("environment", "challenge", {nonce: t.sent[0].payload.cnonce + "c2VydmVy"});
        t.receive("environment", "login", {secret: "s", role: "editor", capabilities: [],
                                           server_signature: Buffer.alloc(32).toString("base64"), resume: "aWQy"});
        expect(t.logins).toEqual([]);
        expect(t.failures).toEqual(["The server could not prove its identity"]);
    });
});
