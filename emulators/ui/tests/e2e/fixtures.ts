// The end-to-end fixtures: every test gets its own Colibri emulator — the real command, on a
// free port, with a fresh library holding the test's environment — serving the UI's build,
// and a page that fails the test on any page error. A test may also use a control client (a
// script's view, newline-delimited JSON on the control socket) and play hakkas on the
// environment link.
import {test as base, expect} from '@playwright/test';
import {type ChildProcess, spawn} from 'node:child_process';
import {mkdirSync, mkdtempSync, rmSync, writeFileSync} from 'node:fs';
import net from 'node:net';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {EMULATOR_MAIN} from './paths';

type Json = null | boolean | number | string | Json[] | {[key: string]: Json};
type JsonObject = {[key: string]: Json};

// A newline-delimited JSON connection over a Unix socket.
class Lines {
    readonly messages: JsonObject[] = [];
    private buffer = '';
    private waiters: (() => void)[] = [];
    readonly socket: net.Socket;

    // (No parameter properties here: Playwright's Node strips types only.)
    constructor(socket: net.Socket) {
        this.socket = socket;
        socket.on('data', d => {
            this.buffer += d.toString();
            let nl;
            while ((nl = this.buffer.indexOf('\n')) >= 0) {
                this.messages.push(JSON.parse(this.buffer.slice(0, nl)));
                this.buffer = this.buffer.slice(nl + 1);
            }
            this.waiters.splice(0).forEach(w => w());
        });
        socket.on('error', () => {});
    }

    static connect(path: string): Promise<Lines> {
        return new Promise((resolve, reject) => {
            const socket = net.connect(path);
            socket.once('connect', () => resolve(new Lines(socket)));
            socket.once('error', reject);
        });
    }

    send(message: object): void {
        this.socket.write(JSON.stringify(message) + '\n');
    }

    // The first message from `from` on that passes `test`.
    async until(test: (m: JsonObject) => boolean, from = 0): Promise<JsonObject> {
        for (;;) {
            const m = this.messages.slice(from).find(test);
            if (m !== undefined)
                return m;
            await new Promise<void>(r => this.waiters.push(r));
        }
    }

    close(): void {
        this.socket.destroy();
    }
}

// A script on the control socket: the state it has seen, and requests.
export class ControlClient {
    private nextId = 1;
    private readonly lines: Lines;

    constructor(lines: Lines) {
        this.lines = lines;
    }

    // Every key's latest value.
    get values(): JsonObject {
        const out: JsonObject = {};
        for (const m of this.lines.messages)
            if ('state' in m)
                Object.assign(out, (m.state as {values: JsonObject}).values);
        return out;
    }

    // Sends a request and waits for its answer.
    async request(name: string, payload: JsonObject = {}): Promise<JsonObject> {
        const id = this.nextId++;
        const from = this.lines.messages.length;
        this.lines.send({[name]: {...payload, id}});
        return this.lines.until(m => ('ok' in m || 'error' in m) && (Object.values(m)[0] as JsonObject).id === id, from);
    }

    async set(key: string, value: Json): Promise<void> {
        const answer = await this.request('set', {key, value});
        expect(answer, `set ${key}`).toHaveProperty('ok');
    }

    close(): void {
        this.lines.close();
    }
}

// hakkas, as far as the emulator can tell: the environment link's client.
export class HakkasClient {
    private readonly lines: Lines;

    constructor(lines: Lines) {
        this.lines = lines;
        lines.send({hello: {version: 1}});
    }

    write(port: string, index: number | undefined, value: number | boolean): void {
        this.lines.send({write: {port, ...(index !== undefined && {index}), value}});
    }

    close(): void {
        this.lines.close();
    }
}

export interface Emulator {
    url: string;
    library: string;        // the library's directory (environments/<name>.json)
    log(): string;
    control(): Promise<ControlClient>;
    hakkas(): Promise<HakkasClient>;
    stop(): Promise<void>;
}

interface Options {
    // The environments in the library, by name; the first runs.
    environments: Record<string, JsonObject>;
    allowedErrors: RegExp[];
}

export const LAB: JsonObject = {
    carrier: 'colibri_8',
    modules: [
        {slot: 1, type: 'colibri_aiv'}, {slot: 2, type: 'colibri_dii'}, {slot: 3, type: 'colibri_ssr'},
        {slot: 4, type: 'colibri_dp1'}, {slot: 5, type: 'colibri_dio1'},
    ],
};

async function stopProcess(proc: ChildProcess): Promise<number | null> {
    if (proc.exitCode !== null)
        return proc.exitCode;
    const exited = new Promise<number | null>(resolve => proc.once('exit', code => resolve(code)));
    proc.kill('SIGINT');
    const timer = setTimeout(() => proc.kill('SIGKILL'), 5000);
    const code = await exited;
    clearTimeout(timer);
    return code;
}

export const test = base.extend<Options & {emulator: Emulator, pageErrors: string[]}>({
    environments: [{lab: LAB}, {option: true}],
    allowedErrors: [[], {option: true}],

    emulator: async ({environments, allowedErrors}, use, testInfo) => {
        const dir = mkdtempSync(join(tmpdir(), 'emu-ui-'));
        mkdirSync(join(dir, 'library', 'environments'), {recursive: true});
        for (const [name, env] of Object.entries(environments))
            writeFileSync(join(dir, 'library', 'environments', `${name}.json`), JSON.stringify(env));
        const socket = join(dir, 'env.sock'), control = join(dir, 'control.sock');
        const proc = spawn(process.execPath, [EMULATOR_MAIN, '--library', join(dir, 'library'), '--socket', socket,
            '--control-socket', control, '--http=0', '--seed', '1', Object.keys(environments)[0]!],
            {stdio: ['ignore', 'ignore', 'pipe']});
        const log: string[] = [];
        let partial = '';
        proc.stderr!.on('data', (d: Buffer) => {
            const lines = (partial + d.toString()).split('\n');
            partial = lines.pop()!;
            log.push(...lines);
        });
        const port = await new Promise<number>((resolve, reject) => {
            const timer = setTimeout(() => reject(new Error('the emulator did not start:\n' + log.join('\n'))), 10000);
            const check = () => {
                const line = log.find(l => l.startsWith('info http: listening on'));
                if (line !== undefined) {
                    clearTimeout(timer);
                    resolve(Number(/:(\d+)\//.exec(line)![1]));
                }
            };
            proc.stderr!.on('data', check);
            proc.once('exit', code => {
                clearTimeout(timer);
                reject(new Error(`the emulator exited (${code}) at startup:\n` + log.join('\n')));
            });
        });
        const opened: {close(): void}[] = [];
        await use({
            url: `http://127.0.0.1:${port}/`,
            library: join(dir, 'library'),
            log: () => log.join('\n'),
            control: async () => {
                const c = new ControlClient(await Lines.connect(control));
                opened.push(c);
                return c;
            },
            hakkas: async () => {
                const h = new HakkasClient(await Lines.connect(socket));
                opened.push(h);
                return h;
            },
            stop: async () => {
                await stopProcess(proc);
            },
        });
        opened.forEach(c => c.close());
        const code = await stopProcess(proc);
        await testInfo.attach('emulator.log', {body: log.join('\n'), contentType: 'text/plain'});
        rmSync(dir, {recursive: true, force: true});
        expect(code, "the emulator's exit code").toBe(0);
        expect(log.filter(l => l.startsWith('error') && !allowedErrors.some(e => e.test(l))),
            "errors in the emulator's log").toEqual([]);
    },

    pageErrors: [async ({page}, use) => {
        const errors: string[] = [];
        page.on('pageerror', e => errors.push('page error: ' + e.message));
        page.on('console', m => {
            if (m.type() === 'error')
                errors.push('console: ' + m.text());
        });
        await use(errors);
        expect(errors, 'errors in the page').toEqual([]);
    }, {auto: true}],
});

export {expect};
