// A second client of the engine, speaking the wire protocol (02) from Node: another user
// editing the same process, or an observer checking what the UI sent.

import WebSocket from "ws";
import {clientNonce, computeProof} from "../../src/components/protocols/scram";

export interface Message {
    protocol: string;
    command: string;
    payload: any;
}

export class ProtocolClient {
    private ws: WebSocket;
    private secret = "";
    private received: Message[] = [];
    private waiters: (() => void)[] = [];

    private constructor(ws: WebSocket) {
        this.ws = ws;
        ws.on("message", raw => {
            this.received.push(JSON.parse(raw.toString()));
            this.waiters.splice(0).forEach(w => w());
        });
    }

    static async connect(port: number, user: string, password: string): Promise<ProtocolClient> {
        const ws = new WebSocket(`ws://127.0.0.1:${port + 1}/`, {origin: `http://127.0.0.1:${port}`});
        await new Promise((resolve, reject) => {
            ws.once("open", resolve);
            ws.once("error", reject);
        });
        const client = new ProtocolClient(ws);
        const cnonce = clientNonce();
        client.send("auth", "challenge", {user, cnonce});
        const challenge = await client.next(m => m.command === "challenge");
        const {proof} = await computeProof(user, password, challenge.payload);
        client.send("auth", "login", {user, nonce: challenge.payload.nonce, proof});
        const login = await client.next(m => m.command === "login" || m.command === "error");
        if (login.command !== "login")
            throw new Error("login failed: " + login.payload.message);
        client.secret = login.payload.secret;
        return client;
    }

    send(protocol: string, command: string, payload: object) {
        this.ws.send(JSON.stringify({protocol, command, payload, secret: this.secret}));
    }

    // The first message received (since the last one taken) that matches; the messages before
    // it are dropped.
    async next(match: (m: Message) => boolean, timeoutMs = 5000): Promise<Message> {
        const end = Date.now() + timeoutMs;
        for (;;) {
            const i = this.received.findIndex(match);
            if (i >= 0)
                return this.received.splice(0, i + 1)[i];
            const left = end - Date.now();
            if (left <= 0)
                throw new Error("no matching message; received: " + JSON.stringify(this.received));
            await new Promise<void>(resolve => {
                const timer = setTimeout(resolve, left);
                this.waiters.push(() => {
                    clearTimeout(timer);
                    resolve();
                });
            });
        }
    }

    // Sends a command and waits for its event (02 §1.1) or a refusal.
    async command(protocol: string, command: string, payload: object): Promise<Message> {
        this.send(protocol, command, payload);
        return this.next(m => (m.protocol === protocol && m.command === command) || m.command === "error");
    }

    // Subscribes to a process (02 §4.1.1) and returns its snapshot.
    async connectGraph(id: string): Promise<Message[]> {
        this.send("graph", "connect", {id});
        await this.next(m => m.command === "clear");
        this.send("graph", "list", {});   // a barrier: answered after the whole snapshot
        const snapshot: Message[] = [];
        for (;;) {
            const m = await this.next(() => true);
            if (m.protocol === "network" && m.command === "status")
                return snapshot;
            snapshot.push(m);
        }
    }

    close() {
        this.ws.close();
    }
}
