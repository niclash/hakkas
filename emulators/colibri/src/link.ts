// The environment link (spec §3, 04 §0.1): the emulator is the server of a Unix socket that
// hakkas connects to. One connection at a time; on each, the description first, then every
// current sensor value, then the changes at each slot's turn. hakkas' greeting is checked and
// its writes go into the modules' queues.
import {mkdir, lstat, unlink} from 'node:fs/promises';
import net from 'node:net';
import path from 'node:path';
import type {Description} from './description.js';
import type {Logger} from './log.js';
import type {Scanner, Turn} from './scan.js';
import type {InputChannel, System} from './system.js';

export const LINE_MAX = 256 * 1024;     // bytes, the newline included
export const WARNINGS_MAX = 8;          // per connection, as hakkas
export const RESTART_CLEAR_MS = 2000;   // a connection open this long clears `restart`
export const VERSION = 1;

export type LinkState = 'waiting' | 'connected' | 'restart';

export interface LinkEvents {
    state?(state: LinkState): void;
    notice?(level: 'info' | 'warning' | 'error', message: string): void;
}

// The environment the link serves: none while the selected one cannot be run.
export interface Served {
    system: System;
    scanner: Scanner;
    description: Description;
}

class Connection {
    private buffer = Buffer.alloc(0);
    private warnings = 0;
    closed = false;

    constructor(readonly socket: net.Socket, private readonly link: LinkServer) {}

    send(message: object): void {
        if (!this.closed)
            this.socket.write(encode(message) + '\n');
    }

    // Splits what arrived into lines; a line over LINE_MAX ends the connection.
    data(chunk: Buffer): void {
        this.buffer = this.buffer.length === 0 ? chunk : Buffer.concat([this.buffer, chunk]);
        let start = 0;
        for (let nl = this.buffer.indexOf(10); nl >= 0 && !this.closed; nl = this.buffer.indexOf(10, start)) {
            if (nl + 1 - start > LINE_MAX)
                return this.end(`a line longer than ${LINE_MAX} bytes: connection closed`);
            this.link.line(this, this.buffer.toString('utf8', start, nl));
            start = nl + 1;
        }
        this.buffer = this.buffer.subarray(start);
        if (this.buffer.length >= LINE_MAX)
            this.end(`a line longer than ${LINE_MAX} bytes: connection closed`);
    }

    warn(message: string): void {
        if (this.warnings++ < WARNINGS_MAX)
            this.link.log.warn(`environment link: ${message}`);
    }

    end(warning?: string): void {
        if (this.closed)
            return;
        if (warning !== undefined)
            this.link.log.warn(`environment link: ${warning}`);
        this.socket.destroy();
        // Gone at once, not at the socket's close event: hakkas may already be reconnecting.
        this.link.closed(this);
    }
}

export class LinkServer {
    private server: net.Server | undefined;
    private connection: Connection | undefined;
    private served: Served | undefined;
    private current: LinkState = 'waiting';
    private described = false;          // a runtime has had the current description
    private restartTimer: NodeJS.Timeout | undefined;
    private warnedNoEnvironment = false;

    constructor(readonly socketPath: string, readonly log: Logger, readonly events: LinkEvents = {}) {}

    get state(): LinkState {
        return this.current;
    }

    // Listens on the socket. A stale socket file is replaced; one another program listens on,
    // or a file that is not a socket, refuses it.
    async listen(): Promise<void> {
        await mkdir(path.dirname(this.socketPath), {recursive: true, mode: 0o700});
        await removeStale(this.socketPath);
        const server = net.createServer(socket => this.accept(socket));
        await new Promise<void>((resolve, reject) => {
            server.once('error', reject);
            server.listen(this.socketPath, () => {
                server.off('error', reject);
                resolve();
            });
        });
        server.on('error', e => this.log.error(`environment link: ${e.message}`));
        this.server = server;
        this.log.info(`environment link: listening on ${this.socketPath}`);
    }

    // Closes the connection and the socket, and removes the socket file.
    async close(): Promise<void> {
        clearTimeout(this.restartTimer);
        this.connection?.end();
        const server = this.server;
        this.server = undefined;
        if (server === undefined)
            return;         // not listening: the file is not ours (maybe another emulator's)
        await new Promise<void>(resolve => server.close(() => resolve()));
        await unlink(this.socketPath).catch(() => {});
    }

    // Serves an environment, or none. Another description than the one a runtime has is a
    // power cycle (spec §5.1): the connection is closed and the runtime must be restarted.
    serve(served: Served | undefined): void {
        const powerCycle = this.described;
        this.served = served;
        this.warnedNoEnvironment = false;
        if (powerCycle) {
            this.described = false;
            this.setState('restart');
        }
        this.connection?.end();
    }

    // A new description that is no power cycle: a communication module's parameters, which
    // hakkas does not compare (04 §0.1) and sees at its next start.
    updateDescription(description: Description): void {
        if (this.served !== undefined)
            this.served = {...this.served, description};
    }

    // A scan turn's changed inputs, sent to the runtime.
    turn(turn: Turn): void {
        const c = this.connection;
        if (c === undefined || turn.changed.length === 0)
            return;
        for (const ch of turn.changed)
            c.send(valueMessage(ch));
    }

    private accept(socket: net.Socket): void {
        if (this.connection !== undefined) {
            socket.destroy();
            const message = 'a second runtime connected: closed (one hakkas per emulator)';
            this.log.warn(`environment link: ${message}`);
            this.events.notice?.('warning', message);
            return;
        }
        if (this.served === undefined) {
            socket.destroy();
            if (!this.warnedNoEnvironment)
                this.log.warn('environment link: no environment to describe: connections are closed');
            this.warnedNoEnvironment = true;
            return;
        }
        const c = new Connection(socket, this);
        this.connection = c;
        socket.on('data', chunk => c.data(chunk));
        socket.on('error', e => this.log.fine(`environment link: ${e.message}`));
        socket.on('close', () => this.closed(c));
        this.log.info('environment link: hakkas connected');
        const {system, description} = this.served;
        c.send({describe: description});
        for (const slot of system.slots)
            for (const ch of slot.inputs)
                if (ch.value !== null)
                    c.send(valueMessage(ch));
        if (this.current === 'restart') {
            this.restartTimer = setTimeout(() => {
                if (this.connection === c)
                    this.setState('connected');
            }, RESTART_CLEAR_MS);
        } else {
            this.setState('connected');
        }
        this.described = true;
    }

    // A connection ended, by either side (once per connection).
    closed(c: Connection): void {
        c.closed = true;
        if (this.connection !== c)
            return;
        this.connection = undefined;
        clearTimeout(this.restartTimer);
        this.log.info('environment link: hakkas disconnected');
        if (this.current === 'connected')
            this.setState('waiting');
    }

    private setState(state: LinkState): void {
        if (state === this.current)
            return;
        this.current = state;
        this.events.state?.(state);
    }

    // One line from hakkas (04 §0.1). A message with an unknown member is ignored.
    line(c: Connection, line: string): void {
        let message: unknown;
        try {
            message = JSON.parse(line);
        } catch {
            return c.warn(`not JSON, ignored: ${excerpt(line)}`);
        }
        if (typeof message !== 'object' || message === null || Array.isArray(message)
            || Object.keys(message).length !== 1)
            return c.warn(`not an object with one member, ignored: ${excerpt(line)}`);
        const [kind, body] = Object.entries(message)[0]!;
        if (kind === 'hello')
            this.hello(c, body);
        else if (kind === 'write')
            this.write(c, body);
    }

    private hello(c: Connection, body: unknown): void {
        const version = isRecord(body) ? body.version : undefined;
        if (version !== VERSION)
            c.end(`hello: version ${JSON.stringify(version) ?? 'missing'}, expected ${VERSION}: connection closed`);
    }

    private write(c: Connection, body: unknown): void {
        if (!isRecord(body) || typeof body.port !== 'string')
            return c.warn('write: expected {port, index, value}, ignored');
        const {port, value} = body;
        const index = body.index ?? undefined;
        const channels = this.served!.system.portChannels(port);
        if (channels === undefined)
            return c.warn(`write: unknown port ${port}, ignored`);
        if (channels[0]!.kind !== 'output')
            return c.warn(`write: ${port} is a sensor port, ignored`);
        const ch = channels.find(x => x.index === index);
        if (ch === undefined || ch.kind !== 'output')
            return c.warn(`write: ${port} has no channel ${JSON.stringify(index) ?? '(none)'}, ignored`);
        let v: number | boolean;
        if (ch.def.type === 'bool' && typeof value === 'boolean')
            v = value;
        else if (ch.def.type === 'number' && (typeof value === 'number' || value === null))
            v = value ?? NaN;           // hakkas writes NaN as null
        else
            return c.warn(`write: ${ch.label}: expected a ${ch.def.type}, ignored`);
        this.served!.scanner.write(ch, v);
    }
}

function valueMessage(ch: InputChannel): object {
    return {value: {port: ch.port, ...(ch.index !== undefined && {index: ch.index}), value: ch.value}};
}

// JSON, with a NaN as null and an infinity as ±1e999 (04 §0.1).
export function encode(message: object): string {
    return JSON.stringify(message, (_, v) =>
        typeof v === 'number' && !Number.isFinite(v) && !Number.isNaN(v) ? `\u0000${v > 0 ? '' : '-'}1e999` : v)
        .replace(/"\\u0000(-?1e999)"/g, '$1');
}

function isRecord(v: unknown): v is Record<string, unknown> {
    return typeof v === 'object' && v !== null && !Array.isArray(v);
}

function excerpt(line: string): string {
    return line.length > 80 ? line.slice(0, 80) + '…' : line;
}

// Removes a socket file nobody listens on; refuses a live one and anything else.
export async function removeStale(file: string): Promise<void> {
    const st = await lstat(file).catch(() => undefined);
    if (st === undefined)
        return;
    if (!st.isSocket())
        throw new Error(`${file}: exists and is not a socket`);
    const live = await new Promise<boolean>(resolve => {
        const probe = net.connect(file);
        probe.once('connect', () => {
            probe.destroy();
            resolve(true);
        });
        probe.once('error', () => resolve(false));
    });
    if (live)
        throw new Error(`${file}: another program is listening on it`);
    await unlink(file);
}
