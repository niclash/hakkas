// The control protocol's transports (control protocol §1): a Unix socket with newline-delimited
// JSON, and the HTTP server on localhost with the WebSocket at /control, the UI's files and the
// plugins' pictures. Both hand every message to the core and send what it sends.
import {createReadStream} from 'node:fs';
import {mkdir, stat, unlink} from 'node:fs/promises';
import http from 'node:http';
import net from 'node:net';
import path from 'node:path';
import {WebSocketServer, type WebSocket} from 'ws';
import type {Catalog} from './catalog.js';
import type {Client, Core} from './core.js';
import {ASSET_NAME, TYPE_NAME} from './descriptors.js';
import {encode, removeStale} from './link.js';
import type {Logger} from './log.js';
import {CARRIER_ASSETS, COMM_ASSETS, MCU_ASSETS, MODULE_ASSETS} from './scene.js';

export const MESSAGE_MAX = 256 * 1024;      // bytes (§1)

// The control socket: one client per connection, one message per line.
export class ControlSocket {
    private server: net.Server | undefined;
    private readonly sockets = new Set<net.Socket>();

    constructor(readonly socketPath: string, private readonly core: Core, private readonly log: Logger) {}

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
        this.server = server;
        this.log.info(`control: listening on ${this.socketPath}`);
    }

    async close(): Promise<void> {
        for (const s of this.sockets)
            s.destroy();
        const server = this.server;
        this.server = undefined;
        if (server === undefined)
            return;         // not listening: the file is not ours (maybe another emulator's)
        await new Promise<void>(resolve => server.close(() => resolve()));
        await unlink(this.socketPath).catch(() => {});
    }

    private accept(socket: net.Socket): void {
        this.sockets.add(socket);
        const client: Client = {send: m => socket.writable && socket.write(encode(m) + '\n')};
        let buffer = Buffer.alloc(0);
        socket.on('data', chunk => {
            buffer = buffer.length === 0 ? chunk : Buffer.concat([buffer, chunk]);
            let start = 0;
            for (let nl = buffer.indexOf(10); nl >= 0; nl = buffer.indexOf(10, start)) {
                if (nl - start > MESSAGE_MAX)
                    return socket.destroy();
                const line = buffer.toString('utf8', start, nl).trim();
                if (line !== '')
                    void this.core.receive(client, line);
                start = nl + 1;
            }
            buffer = buffer.subarray(start);
            if (buffer.length > MESSAGE_MAX)
                socket.destroy();
        });
        socket.on('error', e => this.log.fine(`control: ${e.message}`));
        socket.on('close', () => {
            this.sockets.delete(socket);
            this.core.disconnect(client);
        });
        this.core.connect(client);
    }
}

const TYPES: Record<string, string> = {
    '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.mjs': 'text/javascript; charset=utf-8',
    '.css': 'text/css; charset=utf-8', '.json': 'application/json', '.svg': 'image/svg+xml', '.png': 'image/png',
    '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg', '.webp': 'image/webp', '.ico': 'image/x-icon',
    '.woff2': 'font/woff2', '.map': 'application/json',
};

// The HTTP server, on localhost only (127.0.0.1 and ::1): the WebSocket at /control, the
// pictures under assets/, and the UI's built files (when built).
export class HttpServer {
    private readonly servers: http.Server[] = [];
    private readonly wss = new WebSocketServer({noServer: true, maxPayload: MESSAGE_MAX});
    port = 0;

    constructor(private readonly requestedPort: number, private readonly core: Core,
                private readonly catalog: Catalog, private readonly uiDir: string,
                private readonly log: Logger) {
        this.wss.on('connection', ws => this.accept(ws));
    }

    async listen(): Promise<void> {
        this.port = await this.listenOn('127.0.0.1', this.requestedPort);
        try {
            await this.listenOn('::1', this.port);
        } catch (e) {
            this.log.fine(`http: not on ::1: ${(e as Error).message}`);
        }
        this.log.info(`http: listening on http://localhost:${this.port}/`);
    }

    async close(): Promise<void> {
        for (const ws of this.wss.clients)
            ws.terminate();
        await Promise.all(this.servers.map(s => new Promise<void>(resolve => {
            s.close(() => resolve());
            s.closeAllConnections();
        })));
    }

    private listenOn(host: string, port: number): Promise<number> {
        const server = http.createServer((req, res) => void this.serve(req, res));
        server.on('upgrade', (req, socket, head) => this.upgrade(req, socket as net.Socket, head));
        return new Promise((resolve, reject) => {
            server.once('error', reject);
            server.listen(port, host, () => {
                server.off('error', reject);
                this.servers.push(server);
                resolve((server.address() as net.AddressInfo).port);
            });
        });
    }

    // The UI is served from this origin, so a browser's Origin is one of these; a script sends
    // none (§1).
    private originAllowed(origin: string | undefined): boolean {
        return origin === undefined || ['localhost', '127.0.0.1', '[::1]']
            .some(h => origin === `http://${h}:${this.port}`);
    }

    private upgrade(req: http.IncomingMessage, socket: net.Socket, head: Buffer): void {
        const url = new URL(req.url ?? '/', 'http://localhost');
        const refuse = (status: string) => {
            socket.end(`HTTP/1.1 ${status}\r\nConnection: close\r\nContent-Length: 0\r\n\r\n`);
        };
        if (url.pathname !== '/control')
            return refuse('404 Not Found');
        if (!this.originAllowed(req.headers.origin))
            return refuse('403 Forbidden');
        this.wss.handleUpgrade(req, socket, head, ws => this.wss.emit('connection', ws, req));
    }

    private accept(ws: WebSocket): void {
        const client: Client = {send: m => ws.readyState === ws.OPEN && ws.send(encode(m))};
        ws.on('message', (data, binary) => {
            if (!binary)
                void this.core.receive(client, data.toString());
        });
        ws.on('error', e => this.log.fine(`control: ${e.message}`));
        ws.on('close', () => this.core.disconnect(client));
        this.core.connect(client);
    }

    private async serve(req: http.IncomingMessage, res: http.ServerResponse): Promise<void> {
        if (req.method !== 'GET' && req.method !== 'HEAD') {
            res.writeHead(405, {Allow: 'GET, HEAD'}).end();
            return;
        }
        let pathname: string;
        try {
            pathname = decodeURIComponent(new URL(req.url ?? '/', 'http://localhost').pathname);
        } catch {
            res.writeHead(400).end();
            return;
        }
        const file = this.resolve(pathname);
        const st = file === undefined ? undefined : await stat(file).catch(() => undefined);
        if (file === undefined || st === undefined || !st.isFile()) {
            if (pathname === '/' || pathname === '/index.html') {
                res.writeHead(200, {'Content-Type': 'text/html; charset=utf-8'});
                res.end('<!doctype html><title>Colibri emulator</title><p>The UI is not built: run ' +
                    '<code>npm install &amp;&amp; npm run build</code> in <code>emulators/ui/</code>.</p>\n');
                return;
            }
            res.writeHead(404, {'Content-Type': 'text/plain; charset=utf-8'}).end('Not found\n');
            return;
        }
        res.writeHead(200, {'Content-Type': TYPES[path.extname(file).toLowerCase()] ?? 'application/octet-stream',
            'Content-Length': st.size, 'Cache-Control': 'no-cache'});
        if (req.method === 'HEAD') {
            res.end();
            return;
        }
        createReadStream(file).pipe(res);
    }

    // The file a path names: a plugin's or carrier's picture, or a file of the UI. Nothing
    // outside those directories.
    private resolve(pathname: string): string | undefined {
        const parts = pathname.split('/').filter(p => p !== '');
        const asset = (prefix: string, map: Map<string, {dir: string}>) => {
            const [type, name, ...rest] = parts.slice(prefix.split('/').length);
            if (rest.length > 0 || type === undefined || name === undefined || !TYPE_NAME.test(type)
                || !ASSET_NAME.test(name))
                return undefined;
            const entry = map.get(type);
            return entry === undefined ? undefined : path.join(entry.dir, name);
        };
        const joined = parts.join('/');
        if (joined.startsWith(MODULE_ASSETS + '/'))
            return asset(MODULE_ASSETS, this.catalog.modules);
        if (joined.startsWith(CARRIER_ASSETS + '/'))
            return asset(CARRIER_ASSETS, this.catalog.carriers);
        if (joined.startsWith(COMM_ASSETS + '/'))
            return asset(COMM_ASSETS, this.catalog.comm);
        if (joined.startsWith(MCU_ASSETS + '/'))
            return asset(MCU_ASSETS, this.catalog.mcu);
        if (parts.some(p => p.startsWith('.')))
            return undefined;
        const file = path.join(this.uiDir, ...(parts.length === 0 ? ['index.html'] : parts));
        return file.startsWith(this.uiDir + path.sep) ? file : undefined;
    }
}
