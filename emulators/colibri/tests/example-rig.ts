// A rig for hakkas' examples (hakkas plan Section X, examples/<x>/): the real hakkas running an
// example's process with its bindings against the emulated device of the example's environment
// file. Used by the examples' tests; each skips when hakkas is not built (HAKKAS_BIN, else the
// debug preset's build/debug/hakkas).
import {type ChildProcess, spawn} from 'node:child_process';
import {randomBytes} from 'node:crypto';
import {existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync} from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import {expect} from 'vitest';
import {loadCatalog} from '../src/catalog.js';
import {Checker, type Json, type JsonObject} from '../src/check.js';
import {describe as describeSystem} from '../src/description.js';
import {readEnvironment} from '../src/environment.js';
import {LinkServer} from '../src/link.js';
import {createLogger} from '../src/log.js';
import {BUILTIN_DATA, PACKAGE_ROOT} from '../src/package.js';
import {Scanner} from '../src/scan.js';
import {System, type OutputChannel} from '../src/system.js';

export const HAKKAS = process.env.HAKKAS_BIN ?? path.resolve(PACKAGE_ROOT, '../../build/debug/hakkas');
export const HAKKAS_BUILT = existsSync(HAKKAS);
const catalog = await loadCatalog([BUILTIN_DATA], createLogger('error', () => {}));

type Initial = {src: {data: unknown}, tgt: {block: string, port: string}};

// An example's files: its process file (06 §4.16, one process with its bindings) and its
// emulator environment.
export function readExample(dir: string, processFile: string, environmentFile: string) {
    const root = path.resolve(PACKAGE_ROOT, '../../examples', dir);
    const file = JSON.parse(readFileSync(path.join(root, processFile), 'utf8'));
    return {
        process: file.processes[0] as {graph: JsonObject, bindings: JsonObject},
        types: file.types as JsonObject[],
        environment: JSON.parse(readFileSync(path.join(root, environmentFile), 'utf8')) as JsonObject,
    };
}

// A copy of a graph with some of its fixed values changed, e.g. calendar settings adapted to the
// moment the test runs (hakkas has no clock to set).
export function withValues(graph: JsonObject, values: [string, string, unknown][]): JsonObject {
    const g = structuredClone(graph);
    for (const [block, port, data] of values) {
        const c = (g.connections as Initial[]).find(x => x.tgt.block === block && x.tgt.port === port && 'data' in x.src);
        expect(c, `${block}.${port}`).toBeDefined();
        c!.src.data = data;
    }
    return g;
}

export class ExampleRig {
    system!: System;
    log: string[] = [];
    private tmp = mkdtempSync(path.join(os.tmpdir(), 'colibri-example-'));
    private socketPath = path.join(os.tmpdir(), `ce-${randomBytes(6).toString('hex')}.sock`);
    private link = new LinkServer(this.socketPath, createLogger('error', () => {}));
    private scanner: Scanner | undefined;
    private hakkas: ChildProcess | undefined;

    // `report` names the outputs that a timeout's message lists.
    constructor(private readonly name: string, private readonly environment: JsonObject,
                private readonly report: string[] = []) {
    }

    async listen(): Promise<void> {
        await this.link.listen();
    }

    // The emulated device, its inputs noiseless at the given values.
    serve(inputs: Record<string, number>): void {
        const c = new Checker();
        const env = readEnvironment(c, {...this.environment, inputs: {}})!;
        expect(c.errors).toEqual([]);
        this.system = System.build(this.name, env, catalog, () => performance.now(), Math.random).system;
        for (const [key, v] of Object.entries(inputs)) {
            this.system.set(`${key}.source`, v);
            this.system.set(`${key}.noise`, 0);
        }
        this.scanner = new Scanner(this.system, env, () => performance.now(), createLogger('error', () => {}),
            t => this.link.turn(t));
        this.link.serve({system: this.system, scanner: this.scanner, description: describeSystem(this.system)});
        this.scanner.start();
    }

    // `types`: the composite types the process uses, as the process file holds them.
    async startHakkas(graph: JsonObject, bindings: JsonObject, types: JsonObject[] = []): Promise<void> {
        const conf = path.join(this.tmp, 'conf'), data = path.join(this.tmp, 'data');
        mkdirSync(conf);
        mkdirSync(path.join(data, 'graphs'), {recursive: true});
        mkdirSync(path.join(data, 'bindings'));
        mkdirSync(path.join(data, 'types'));
        for (const t of types)
            writeFileSync(path.join(data, 'types', `${encodeURIComponent(String(t.id))}.json`), JSON.stringify(typeFile(t)));
        writeFileSync(path.join(conf, 'hakkas.json'), JSON.stringify({
            auth: {server_secret: randomBytes(32).toString('base64')}, environment: {socket: this.socketPath}}));
        writeFileSync(path.join(data, 'graphs', `${graph.id}.json`), JSON.stringify(graph));
        writeFileSync(path.join(data, 'bindings', `${graph.id}.json`), JSON.stringify(bindings));
        const port = await freePortPair();
        const proc = spawn(HAKKAS, ['--info', '--confdir', conf, '--basedir', data, 'run',
            '--host', '127.0.0.1', '--port', String(port), '--webdir', this.tmp], {stdio: ['ignore', 'pipe', 'pipe']});
        this.hakkas = proc;
        let partial = '';
        const collect = (d: Buffer) => {
            const lines = (partial + d.toString()).split('\n');
            partial = lines.pop()!;
            this.log.push(...lines.filter(l => l !== ''));
        };
        proc.stdout!.on('data', collect);
        proc.stderr!.on('data', collect);
        await this.until(() => this.log.some(l => / process(es)? loaded/.test(l)), 'hakkas to start');
    }

    // The value of an output channel, e.g. 'slot6.dq.1' or 'slot2.dq'.
    out(key: string): unknown {
        return (this.system.item(key) as OutputChannel).value;
    }

    // hakkas' warnings and errors, but for the ones every such run has.
    problems(): string[] {
        return this.log.filter(l => /^(warn|error)/.test(l) && !/users\.json not found|no frontend in/.test(l));
    }

    async until(condition: () => boolean, what: string, ms = 8000): Promise<void> {
        const end = Date.now() + ms;
        while (!condition()) {
            if (Date.now() > end) {
                const outputs = this.report.map(k => `${k}=${String(this.out(k))}`).join(' ');
                throw new Error(`timed out waiting for ${what}: ${outputs}\nhakkas:\n${this.log.join('\n')}`);
            }
            await new Promise(r => setTimeout(r, 10));
        }
    }

    async close(): Promise<void> {
        const proc = this.hakkas;
        this.hakkas = undefined;
        if (proc !== undefined && proc.exitCode === null) {
            const exited = new Promise(resolve => proc.once('exit', resolve));
            proc.kill('SIGINT');
            await exited;
        }
        this.scanner?.stop();
        this.scanner = undefined;
        await this.link.close();
        rmSync(this.tmp, {recursive: true, force: true});
    }
}

type Ref = {block: string, port: string};

// A composite type's file (hakkas 03 §5.1) from its component graph, the form a process file holds
// it in (06 §4.16).
function typeFile(t: JsonObject): JsonObject {
    const g = t as unknown as {id: string, description: string, icon: string,
        blocks: Record<string, {component: string}>, connections: {src: Ref | {data: unknown}, tgt: Ref}[],
        inports: (Ref & {name: string})[], outports: (Ref & {name: string})[]};
    const ref = (r: Ref) => `${r.block}.${r.port}`;
    return {
        name: g.id, icon: g.icon, description: g.description,
        blocks: Object.entries(g.blocks).map(([name, b]) => ({name, type: b.component})),
        connections: g.connections.filter(c => !('data' in c.src)).map(c => ({src: ref(c.src as Ref), tgt: ref(c.tgt)})),
        initials: g.connections.filter(c => 'data' in c.src).map(c => ({src: (c.src as {data: Json}).data, tgt: ref(c.tgt)})),
        inports: g.inports.map(p => ({name: p.name, tgt: ref(p)})),
        outports: g.outports.map(p => ({name: p.name, src: ref(p)})),
    };
}

async function freePortPair(): Promise<number> {
    const free = (port: number) => new Promise<boolean>(resolve => {
        const s = net.createServer().once('error', () => resolve(false))
            .listen(port, '127.0.0.1', () => s.close(() => resolve(true)));
    });
    for (;;) {
        const port = 20000 + Math.floor(Math.random() * 40000);
        if (await free(port) && await free(port + 1))
            return port;
    }
}
