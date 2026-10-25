import {Connection} from "@/components/protocols/websocket";
import {Challenge, clientNonce, computeProof, computeResumeProof} from "@/components/protocols/scram";
import {KeptSession} from "@/session";

export interface OnLogin {
    user: string;
    secret: string;
    role: string;
    capabilities: string[];
    // What resumes this session (02 §2.3.1); absent if the server issued no resume id.
    resume?: KeptSession;
}

interface Pending {
    user: string;
    cnonce: string;
    password: string;           // a password login; "" once the proof is computed
    resume?: KeptSession;       // a resume
    serverSignature?: string;
    resumeKey?: Uint8Array;
}

export interface OnLoginFailed {
    message: string;
}

// The challenge-response login of specifications/02-wire-protocol.md §2.3, and resuming a
// session (§2.3.1). A server refusal arrives as network:error "Login failed", which the network
// protocol reports.
export class AuthProtocol {
    private connection: Connection;
    private pending: Pending | undefined;
    listeners: {
        onLogin: Function[],
        onLoginFailed: Function[],
        onLogout: Function[],
    };

    constructor(connection: Connection) {
        this.connection = connection;
        this.listeners = {
            onLogin: [],
            onLoginFailed: [],
            onLogout: []
        };
    }

    addListener( method: keyof AuthProtocol['listeners'], listener: Function ): void {
        if (!this.listeners[method]) {
            this.listeners[method] = [];
        }
        this.listeners[method].push(listener);
    }

    addListeners( protocolListeners: Record<string, Function[]> ): void {
        let list = this.listeners;
        Object.entries(list).forEach( ([method, array]) => {
            let fns: Function[] = protocolListeners[method];
            if( fns !== undefined )
                array.push(...fns);
        } );
    };

    execute(command: string, payload: any) {
        switch (command) {
            case "challenge": this.onChallenge(payload); break;
            case "login": this.onLogin(payload); break;
            case "logout": this.onLogout(); break;
        }
    }

    // Starts the handshake; the password is kept only until the proof is computed.
    request_login(user: string, password: string) {
        const cnonce = clientNonce();
        this.pending = {user: user, password: password, cnonce: cnonce};
        this.connection.send({
            protocol: "auth",
            command: "challenge",
            payload: {
                user: user,
                cnonce: cnonce
            }
        });
    }

    // Resumes a kept session (02 §2.3.1); its id is used up whatever the outcome.
    request_resume(session: KeptSession) {
        const cnonce = clientNonce();
        this.pending = {user: session.user, password: "", cnonce: cnonce, resume: session};
        this.connection.send({
            protocol: "auth",
            command: "challenge",
            payload: {
                user: session.user,
                cnonce: cnonce,
                resume: session.id
            }
        });
    }

    request_logout() {
        this.connection.send({
            protocol: "auth",
            command: "logout",
            payload: {}
        });
    }

    // Forgets an unfinished handshake, e.g. after the server refused it.
    abandon_login() {
        this.pending = undefined;
    }

    private async onChallenge(payload: Challenge) {
        const pending = this.pending;
        if (pending === undefined || typeof payload.nonce !== "string" || !payload.nonce.startsWith(pending.cnonce)) {
            this.failed("Unexpected login challenge");
            return;
        }
        if (pending.resume !== undefined) {
            const session = pending.resume;
            const result = computeResumeProof(session.user, payload.nonce, session.id, session.key);
            pending.serverSignature = result.serverSignature;
            pending.resumeKey = session.key;
            this.connection.send({
                protocol: "auth",
                command: "login",
                payload: {
                    user: session.user,
                    nonce: payload.nonce,
                    resume: session.id,
                    proof: result.proof
                }
            });
            return;
        }
        let result;
        try {
            result = await computeProof(pending.user, pending.password, payload);
        } catch (e) {
            this.failed("Login failed");
            return;
        }
        if (this.pending !== pending)
            return;     // abandoned meanwhile
        pending.password = "";
        pending.serverSignature = result.serverSignature;
        pending.resumeKey = result.resumeKey;
        this.connection.send({
            protocol: "auth",
            command: "login",
            payload: {
                user: pending.user,
                nonce: payload.nonce,
                proof: result.proof
            }
        });
    }

    private onLogin(payload: any) {
        const pending = this.pending;
        this.pending = undefined;
        if (pending === undefined || pending.serverSignature !== payload.server_signature) {
            // The device did not prove that it knows the user's server key.
            this.failed("The server could not prove its identity");
            return;
        }
        const login: OnLogin = {user: pending.user, secret: payload.secret, role: payload.role, capabilities: payload.capabilities};
        if (typeof payload.resume === "string" && pending.resumeKey !== undefined)
            login.resume = {user: pending.user, id: payload.resume, key: pending.resumeKey};
        for (let fn of this.listeners.onLogin) {
            fn(this.connection, login);
        }
    }

    private onLogout() {
        this.pending = undefined;
        for (let fn of this.listeners.onLogout) {
            fn(this.connection);
        }
    }

    private failed(message: string) {
        this.pending = undefined;
        for (let fn of this.listeners.onLoginFailed) {
            fn(this.connection, {message: message} as OnLoginFailed);
        }
    }
}
