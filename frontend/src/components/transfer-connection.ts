// The transfer's own connection (06 §4.16): a second connection of the session, resumed with the
// kept session (02 §2.3.1), so that reading and uploading processes leave the editor's
// connection, its scope and its watches alone. It reads graphs by their snapshots and sends the
// upload's commands; what the engine refuses is collected per graph.

import {Connection} from "@/components/protocols/websocket";
import {OnLogin} from "@/components/protocols/auth";
import type {Binding} from "@/components/protocols/environment";
import {keepSession, keptSession} from "@/session";
import {GraphFile, graphFromSnapshot, SnapshotMessage, TypeFile} from "@/components/transfer";

// How long one step may take before the transfer gives up (a lost connection, an engine that
// does not answer).
const TIMEOUT_MS = 15000;

export class TransferError extends Error {
}

type Listener = (protocol: string, command: string, payload: any) => boolean;

export class TransferConnection {
    private readonly conn: Connection;
    private listeners: Listener[] = [];
    private syncs = 0;
    private closed = false;
    // The refusals of each graph's commands since `refusals(graph)` was last asked.
    private errors = new Map<string, string[]>();

    private constructor(conn: Connection) {
        this.conn = conn;
        conn.messageListeners.push((protocol, command, payload) => {
            if (protocol === "network" && command === "error" && typeof payload?.graph === "string")
                this.errors.set(payload.graph, [...(this.errors.get(payload.graph) ?? []), String(payload.message)]);
            this.listeners = this.listeners.filter(l => !l(protocol, command, payload));
        });
        conn.addClosedListener(() => {
            this.closed = true;
            this.listeners.forEach(l => l("", "closed", {}));
            this.listeners = [];
        });
    }

    // A logged-in second connection, or a TransferError saying why there is none.
    static open(): Promise<TransferConnection> {
        const session = keptSession();
        if (session === undefined)
            return Promise.reject(new TransferError("The browser keeps no session to open a second connection with: log in again"));
        const conn = new Connection();
        const t = new TransferConnection(conn);
        return new Promise((resolve, reject) => {
            const fail = (message: string) => {
                conn.close();
                reject(new TransferError(message));
            };
            const timer = setTimeout(() => fail("The engine did not answer"), TIMEOUT_MS);
            conn.proto.auth.addListener("onLogin", (_: Connection, login: OnLogin) => {
                clearTimeout(timer);
                // The id just used is gone: keep the new one, as the editor's connection does.
                if (login.resume !== undefined)
                    keepSession(login.resume);
                conn.currentSecret = login.secret;
                resolve(t);
            });
            conn.proto.auth.addListener("onLoginFailed", (_: Connection, f: { message: string }) => {
                clearTimeout(timer);
                fail(f.message);
            });
            conn.messageListeners.push((protocol, command, payload) => {
                if (conn.currentSecret === undefined && protocol === "network" && command === "error") {
                    clearTimeout(timer);
                    fail("The second connection could not log in: " + payload?.message);
                }
            });
            conn.addClosedListener(() => {
                clearTimeout(timer);
                reject(new TransferError("The connection to the engine was lost"));
            });
            conn.addOpenedListener(() => conn.proto.auth.request_resume(session));
            conn.open();
        });
    }

    // Closed without a logout, which would end the session's chain (02 §2.3.1).
    close() {
        this.conn.close();
    }

    send(protocol: string, command: string, payload: Record<string, unknown>) {
        this.conn.send({protocol, command, payload});
    }

    // Resolves with the first message `pick` takes (returns non-undefined for); rejects when the
    // connection closes or nothing comes in time.
    waitFor<T>(pick: (protocol: string, command: string, payload: any) => T | undefined): Promise<T> {
        if (this.closed)
            return Promise.reject(new TransferError("The connection to the engine was lost"));
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                this.listeners = this.listeners.filter(l => l !== listener);
                reject(new TransferError("The engine did not answer in time"));
            }, TIMEOUT_MS);
            const listener: Listener = (protocol, command, payload) => {
                if (command === "closed" && protocol === "") {
                    clearTimeout(timer);
                    reject(new TransferError("The connection to the engine was lost"));
                    return true;
                }
                const v = pick(protocol, command, payload);
                if (v === undefined)
                    return false;
                clearTimeout(timer);
                resolve(v);
                return true;
            };
            this.listeners.push(listener);
        });
    }

    // Whether a message `match` takes has come since; `stop()` stops looking.
    seen(match: (protocol: string, command: string, payload: any) => boolean): { readonly value: boolean, stop: () => void } {
        const state = {value: false, stop: () => {}};
        const listener: Listener = (protocol, command, payload) => {
            if (!match(protocol, command, payload))
                return false;
            state.value = true;
            return true;
        };
        state.stop = () => this.listeners = this.listeners.filter(l => l !== listener);
        this.listeners.push(listener);
        return state;
    }

    // Resolves once everything sent before it has been answered: an empty network:edges, whose
    // echo comes to this connection only, after the replies and events of what came before.
    sync(): Promise<void> {
        const token = `transfer-sync-${++this.syncs}`;
        const done = this.waitFor((p, c, payload) => p === "network" && c === "edges" && payload?.graph === token ? true : undefined);
        this.send("network", "edges", {graph: token, edges: []});
        return done.then(() => undefined);
    }

    // The refusals of the graph's commands collected so far, and forgotten.
    refusals(graph: string): string[] {
        const e = this.errors.get(graph) ?? [];
        this.errors.delete(graph);
        return e;
    }

    // A process or a type as the engine has it (its graph:connect snapshot, 02 §4.1.1).
    async read(id: string): Promise<GraphFile | TypeFile> {
        const messages: SnapshotMessage[] = [];
        let started = false;
        const collect = (protocol: string, command: string, payload: any) => {
            if (protocol !== "graph")
                return;
            if (command === "clear" && payload?.id === id)
                started = true;
            if (started && (command === "clear" || payload?.graph === id))
                messages.push({command, payload});
        };
        this.conn.messageListeners.push(collect);
        try {
            this.refusals(id);
            const outcome = this.waitFor((protocol, command, payload) => {
                if (protocol === "graph" && command === "clear" && payload?.id === id)
                    return "clear";
                if (protocol === "network" && command === "error" && payload?.graph === id)
                    return "error:" + payload.message;
                return undefined;
            });
            this.send("graph", "connect", {id});
            const first = await outcome;
            if (first.startsWith("error:"))
                throw new TransferError(first.substring(6));
            await this.sync();
        } finally {
            this.conn.messageListeners.splice(this.conn.messageListeners.indexOf(collect), 1);
        }
        const g = graphFromSnapshot(messages);
        if (g === undefined)
            throw new TransferError("Unknown graph: " + id);
        return g;
    }

    // A composite type's source, its type file's JSON (02 §3.5 getsource).
    async source(name: string): Promise<string> {
        const got = this.waitFor((protocol, command, payload) => {
            if (protocol === "component" && command === "source" && payload?.name === name)
                return String(payload.code ?? "");
            if (protocol === "network" && command === "error" && payload?.graph === undefined)
                return "";
            return undefined;
        });
        this.send("component", "getsource", {name});
        return got;
    }

    // Registers a type file (02 §3.5 component:source): "" when the engine announced the type,
    // else its refusal.
    addSource(name: string, code: string): Promise<string> {
        const got = this.waitFor((protocol, command, payload) => {
            if (protocol === "component" && command === "component" && payload?.name === name)
                return "";
            if (protocol === "network" && command === "error" && payload?.graph === undefined)
                return String(payload.message);
            return undefined;
        });
        this.send("component", "source", {name, language: "json", library: "", code});
        return got;
    }

    // Every process's bindings, or undefined for an engine without an environment.
    async bindings(): Promise<Binding[] | undefined> {
        const got = this.waitFor((protocol, command, payload) => {
            if (protocol === "environment" && command === "bindings")
                return (payload?.bindings ?? []) as Binding[];
            if (protocol === "network" && command === "error" && payload?.message === "No environment")
                return null;
            return undefined;
        });
        this.send("environment", "getbindings", {});
        return (await got) ?? undefined;
    }
}
