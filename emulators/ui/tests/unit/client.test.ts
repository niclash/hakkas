import {afterEach, beforeEach, describe, expect, it, vi} from 'vitest';
import {Client, RETRY_MS, type Socket} from '@/protocol/client';

// A WebSocket the test drives: what the client sent, and messages from the "emulator".
class FakeSocket implements Socket {
    onopen: (() => void) | null = null;
    onclose: (() => void) | null = null;
    onmessage: ((e: {data: unknown}) => void) | null = null;
    sent: unknown[] = [];
    closed = false;

    send(data: string): void {
        this.sent.push(JSON.parse(data));
    }

    close(): void {
        this.closed = true;
        this.onclose?.();
    }

    receive(message: object): void {
        this.onmessage?.({data: JSON.stringify(message)});
    }
}

let sockets: FakeSocket[];
let client: Client;
const last = () => sockets.at(-1)!;

function connected(): FakeSocket {
    const s = last();
    s.onopen?.();
    s.receive({hello: {protocol: 1, emulator: 'colibri', version: '0.1.0', features: []}});
    return s;
}

beforeEach(() => {
    vi.useFakeTimers();
    sockets = [];
    client = new Client('ws://x/control', () => {
        const s = new FakeSocket();
        sockets.push(s);
        return s;
    });
    client.start();
});

afterEach(() => {
    client.stop();
    vi.useRealTimers();
});

describe('the client', () => {
    it('mirrors what the emulator sends', () => {
        const s = connected();
        const m = client.mirror;
        expect(m.connection).toBe('open');
        s.receive({library: {environments: [{name: 'lab'}], running: 'lab', unsaved: false}});
        s.receive({scene: {board: null, parts: [{id: 'slot1', place: 'slot-1', type: 'colibri_aiv', name: 'Colibri AIV'}], rows: []}});
        s.receive({state: {values: {'slot1.ai.1': 1, 'slot1.ai.1.source': 1}}});
        s.receive({state: {values: {'slot1.ai.1': 2}}});
        s.receive({stats: {parts: {slot1: {queue: 0, queueMax: 0, dropped: 0}}, channels: {a: {sent: 1}}}});
        s.receive({stats: {parts: {}, channels: {b: {sent: 2}}}});
        s.receive({link: {state: 'connected'}});
        expect(m.library?.running).toBe('lab');
        expect(m.scene.parts[0]?.name).toBe('Colibri AIV');
        expect(m.values).toEqual({'slot1.ai.1': 2, 'slot1.ai.1.source': 1});
        expect(m.stats.channels).toEqual({a: {sent: 1}, b: {sent: 2}});
        expect(m.link).toBe('connected');
    });

    it('answers a request with the emulator\'s ok or error, matched by id', async () => {
        const s = connected();
        const a = client.request('set', {key: 'k', value: 1});
        const b = client.request('persist');
        expect(s.sent).toEqual([{set: {key: 'k', value: 1, id: 1}}, {persist: {id: 2}}]);
        s.receive({error: {id: 2, request: 'persist', message: 'No environment'}});
        s.receive({ok: {id: 1}});
        await expect(a).resolves.toEqual({});
        await expect(b).rejects.toThrow('No environment');
    });

    it('refuses a request while not connected', async () => {
        await expect(client.request('set')).rejects.toThrow('Not connected');
    });

    it('shows notices until dismissed', () => {
        const s = connected();
        s.receive({notice: {level: 'warning', message: 'a second runtime connected'}});
        client.notify({level: 'error', message: 'Refused'});
        expect(client.mirror.notices.map(n => n.message)).toEqual(['a second runtime connected', 'Refused']);
        client.dismiss(client.mirror.notices[0]!.id);
        expect(client.mirror.notices.map(n => n.message)).toEqual(['Refused']);
    });

    it('reconnects after a lost connection, failing what was pending, and starts over from hello', async () => {
        const s = connected();
        s.receive({state: {values: {old: 1}}});
        const pending = client.request('set', {key: 'k', value: 1});
        s.onclose?.();
        await expect(pending).rejects.toThrow('Disconnected');
        expect(client.mirror.connection).toBe('closed');
        expect(sockets).toHaveLength(1);
        vi.advanceTimersByTime(RETRY_MS);
        expect(sockets).toHaveLength(2);
        expect(client.mirror.connection).toBe('connecting');
        connected();
        expect(client.mirror.values).toEqual({});
        expect(client.mirror.connection).toBe('open');
    });

    it('stops at another protocol version', () => {
        last().onopen?.();
        last().receive({hello: {protocol: 2, emulator: 'x', version: '9', features: []}});
        expect(client.mirror.connection).toBe('incompatible');
        expect(last().closed).toBe(true);
        vi.advanceTimersByTime(RETRY_MS * 3);
        expect(sockets).toHaveLength(1);
    });

    it('ignores what is not a message', () => {
        const s = connected();
        s.onmessage?.({data: 'nonsense'});
        s.onmessage?.({data: '[1]'});
        s.receive({ok: {id: 99}});
        expect(client.mirror.connection).toBe('open');
    });
});
