import {Connection} from "@/components/protocols/websocket";
import {Challenge, clientNonce, computeProof} from "@/components/protocols/scram";

export interface OnLogin {
    secret: string;
    role: string;
    capabilities: string[];
}

export interface OnLoginFailed {
    message: string;
}

// The challenge-response login of specifications/02-wire-protocol.md §2.3. A server refusal
// arrives as network:error "Login failed", which the network protocol reports.
export class EnvironmentProtocol {
    private connection: Connection;
    private pending: { user: string, password: string, cnonce: string, serverSignature?: string } | undefined;
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

    addListener( method: keyof EnvironmentProtocol['listeners'], listener: Function ): void {
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
            protocol: "environment",
            command: "challenge",
            payload: {
                user: user,
                cnonce: cnonce
            }
        });
    }

    request_logout() {
        this.connection.send({
            protocol: "environment",
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
        this.connection.send({
            protocol: "environment",
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
        const login: OnLogin = {secret: payload.secret, role: payload.role, capabilities: payload.capabilities};
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
