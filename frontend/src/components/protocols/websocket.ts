import {Protocols} from "@/components/protocols/protocols";

export interface Packet {
    protocol: string;
    command: string;
    payload: any;
    secret?: string;
}

// The FBP WebSocket listens on the web server's port + 1 (specifications/02-wire-protocol.md §0).
// VITE_HAKKAS_WS overrides it for `npm run dev`, whose page is served by Vite instead.
function websocketUrl(): string {
    const override = import.meta.env.VITE_HAKKAS_WS as string | undefined;
    if (override)
        return override;
    const scheme = location.protocol === "https:" ? "wss://" : "ws://";
    const port = location.port !== "" ? Number(location.port) : (location.protocol === "https:" ? 443 : 80);
    const host = location.hostname.includes(":") ? "[" + location.hostname + "]" : location.hostname;
    return scheme + host + ":" + (port + 1) + "/";
}

// Logged without anything secret: the session secret, nonces and proofs of the login.
function loggable(protocol: string, command: string, payload: any): string {
    return protocol + ":" + command + (protocol === "environment" ? "" : " " + JSON.stringify(payload));
}

export class Connection {
    private underlying: (WebSocket | undefined);
    currentSecret: string | undefined;
    currentGraph: string;
    readonly proto: Protocols;
    openListeners: ((conn: Connection, event: Event) => void)[] = [];
    closeListeners: ((conn: Connection, event: CloseEvent) => void)[] = [];
    errorListeners: ((conn: Connection, event: Event) => void)[] = [];

    constructor() {
        console.log("Creating Connection...");
        this.proto = new Protocols(this);
        this.currentGraph = "";
    }

    open() {
        this.underlying = new WebSocket(websocketUrl());
        this.underlying.onopen = (message: Event) => this.onOpen(message);
        this.underlying.onclose = (message: CloseEvent) => this.onClose(message);
        this.underlying.onerror = (message: Event) => this.onError(message);
        this.underlying.onmessage = (message: MessageEvent) => this.onMessage(message);
    }

    close() {
        this.underlying?.close();
    }

    onOpen(message: Event) {
        console.log("WebSocket.onOpen", JSON.stringify(message));
        this.openListeners.forEach(listener => {
            try {
                listener(this, message);
            } catch (e) {
                console.log("Error in listener: ", listener.name, e);
            }
        });
    }

    onClose(message: CloseEvent) {
        console.log("WebSocket.onClose()", JSON.stringify(message));
        this.closeListeners.forEach(listener => {
            try {
                listener(this, message);
            } catch (e) {
                console.log("Error in listener: ", listener.name, e);
            }
        });
    }

    onError(message: Event) {
        console.log("WebSocket.onError()", JSON.stringify(message));
        this.errorListeners.forEach(listener => {
            try {
                listener(this, message);
            } catch (e) {
                console.log("Error in listener: ", listener.name, e);
            }
        });
    }

    onMessage(message: MessageEvent) {
        let data = JSON.parse(message.data);
        let protocol = data.protocol;
        let command = data.command;
        let payload = data.payload;
        console.log("==>", loggable(protocol, command, payload));

        // graph.startTransaction(protocol + " " + command);
        // viewModel.skipsUndoManager = true;
        // graph.skipsUndoManager = true;
        try {
            this.proto.onMessage(protocol, command, payload);
        } catch (e) {
            console.log("Protocol command not found: ", protocol, command, e);
        } finally {
            // viewModel.skipsUndoManager = false;
            // graph.skipsUndoManager = false;
            // graph.commitTransaction(protocol + " " + command);
        }
    }

    send(packet: Packet): void {
        packet.secret = this.currentSecret ? this.currentSecret : "";
        console.log("<==", loggable(packet.protocol, packet.command, packet.payload));
        let data = JSON.stringify(packet);
        this.underlying?.send(data);
    }

    addOpenedListener(listener: (conn: Connection, message: Event) => void) {
        this.openListeners.push(listener);
    }

    addClosedListener(listener: (conn: Connection, message: CloseEvent) => void) {
        this.closeListeners.push(listener);
    }

    addErrorListener(listener: (conn: Connection, message: Event) => void) {
        this.errorListeners.push(listener);
    }
}
