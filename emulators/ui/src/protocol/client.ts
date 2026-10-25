// The control protocol's client (01-control-protocol.md): one WebSocket to the emulator that
// served the page, a mirror of its state, and requests with their answers. The mirror changes
// only from what the emulator sends (the principle of 00-README.md): a request changes nothing
// here by itself.
import {reactive} from 'vue';
import type {
    Hello, Json, JsonObject, Library, LinkState, Notice, Scene, Settings, Stats,
} from './types';

export const PROTOCOL = 1;
export const RETRY_MS = 1000;

export type ConnectionState = 'connecting' | 'open' | 'closed' | 'incompatible';

export interface Mirror {
    connection: ConnectionState;
    hello: Hello | null;
    library: Library | null;
    settings: Settings;
    scene: Scene;
    values: Record<string, Json>;
    stats: Stats;
    link: LinkState | null;
    notices: (Notice & {id: number})[];
}

// What the client needs of a WebSocket (the browser's, or a fake in tests).
export interface Socket {
    onopen: (() => void) | null;
    onclose: (() => void) | null;
    onmessage: ((e: {data: unknown}) => void) | null;
    send(data: string): void;
    close(): void;
}

export class RequestError extends Error {
    constructor(readonly request: string, message: string) {
        super(message);
    }
}

interface Pending {
    request: string;
    resolve: (result: JsonObject) => void;
    reject: (e: Error) => void;
}

function empty(): Omit<Mirror, 'connection' | 'notices'> {
    return {hello: null, library: null, settings: {fields: [], values: {}}, scene: {board: null, parts: [], rows: []},
        values: {}, stats: {parts: {}, channels: {}}, link: null};
}

export class Client {
    readonly mirror: Mirror = reactive({connection: 'connecting', notices: [], ...empty()}) as Mirror;
    private socket: Socket | undefined;
    private nextId = 1;
    private nextNotice = 1;
    private readonly pending = new Map<number, Pending>();
    private retry: ReturnType<typeof setTimeout> | undefined;
    private stopped = false;

    constructor(private readonly url: string, private readonly open: (url: string) => Socket) {}

    start(): void {
        this.stopped = false;
        this.connect();
    }

    stop(): void {
        this.stopped = true;
        clearTimeout(this.retry);
        this.socket?.close();
    }

    // Sends a request; resolves with the `ok` payload (its id removed), rejects with the
    // emulator's refusal.
    request(name: string, payload: JsonObject = {}): Promise<JsonObject> {
        const socket = this.socket;
        if (socket === undefined || this.mirror.connection !== 'open')
            return Promise.reject(new RequestError(name, 'Not connected'));
        const id = this.nextId++;
        return new Promise((resolve, reject) => {
            this.pending.set(id, {request: name, resolve, reject});
            socket.send(JSON.stringify({[name]: {...payload, id}}));
        });
    }

    // A notice the UI shows until it is dismissed: the emulator's, or a refusal of a request.
    notify(notice: Notice): void {
        this.mirror.notices.push({...notice, id: this.nextNotice++});
    }

    dismiss(id: number): void {
        const i = this.mirror.notices.findIndex(n => n.id === id);
        if (i >= 0)
            this.mirror.notices.splice(i, 1);
    }

    private connect(): void {
        this.mirror.connection = 'connecting';
        const socket = this.open(this.url);
        this.socket = socket;
        socket.onopen = () => {
            if (this.socket === socket)
                this.mirror.connection = 'open';
        };
        socket.onmessage = e => {
            if (this.socket !== socket || typeof e.data !== 'string')
                return;
            let message: unknown;
            try {
                message = JSON.parse(e.data);
            } catch {
                return;
            }
            if (typeof message === 'object' && message !== null && !Array.isArray(message))
                this.handle(message as JsonObject);
        };
        socket.onclose = () => {
            if (this.socket !== socket)
                return;
            this.socket = undefined;
            for (const p of this.pending.values())
                p.reject(new RequestError(p.request, 'Disconnected'));
            this.pending.clear();
            if (this.mirror.connection === 'incompatible' || this.stopped)
                return;
            this.mirror.connection = 'closed';
            this.retry = setTimeout(() => this.connect(), RETRY_MS);
        };
    }

    // One message from the emulator (§3).
    handle(message: JsonObject): void {
        const [name, body] = Object.entries(message)[0] ?? [];
        if (name === undefined || typeof body !== 'object' || body === null || Array.isArray(body))
            return;
        const m = this.mirror;
        switch (name) {
        case 'hello': {
            const hello = body as unknown as Hello;
            Object.assign(m, empty());
            m.hello = hello;
            if (hello.protocol !== PROTOCOL) {
                m.connection = 'incompatible';
                this.socket?.close();
            }
            break;
        }
        case 'library':
            m.library = body as unknown as Library;
            break;
        case 'settings':
            m.settings = body as unknown as Settings;
            break;
        case 'scene':
            m.scene = body as unknown as Scene;
            break;
        case 'state':
            Object.assign(m.values, (body as {values: JsonObject}).values);
            break;
        case 'stats': {
            const s = body as unknown as Stats;
            Object.assign(m.stats.parts, s.parts);
            Object.assign(m.stats.channels, s.channels);
            break;
        }
        case 'link':
            m.link = (body as {state: LinkState}).state;
            break;
        case 'notice':
            this.notify(body as unknown as Notice);
            break;
        case 'ok':
        case 'error': {
            const {id, ...rest} = body as JsonObject;
            const p = typeof id === 'number' ? this.pending.get(id) : undefined;
            if (p === undefined)
                return;
            this.pending.delete(id as number);
            if (name === 'ok')
                p.resolve(rest);
            else
                p.reject(new RequestError(p.request, String(rest.message)));
            break;
        }
        }
    }
}
