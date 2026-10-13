// A Connection that never opens a socket: messages are fed in through its real dispatch
// (onMessage), and what it sends is recorded.

import {Connection, Packet} from "@/components/protocols/websocket";

export interface TestConnection {
    conn: Connection;
    sent: Packet[];
    receive: (protocol: string, command: string, payload?: unknown) => void;
}

export function testConnection(): TestConnection {
    const conn = new Connection();
    const sent: Packet[] = [];
    conn.send = (packet: Packet) => {
        sent.push({...packet, secret: conn.currentSecret ?? ""});
    };
    const receive = (protocol: string, command: string, payload?: unknown) => {
        const message: Record<string, unknown> = {protocol, command};
        if (payload !== undefined)
            message.payload = payload;
        conn.onMessage({data: JSON.stringify(message)} as MessageEvent);
    };
    return {conn, sent, receive};
}

export async function until(condition: () => boolean, timeoutMs = 5000) {
    const end = Date.now() + timeoutMs;
    while (!condition()) {
        if (Date.now() > end)
            throw new Error("timed out");
        await new Promise(r => setTimeout(r, 5));
    }
}
