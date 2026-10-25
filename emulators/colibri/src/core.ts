// The emulator's core (control protocol, spec §5.1, §8): the running environment with its
// emulated system, scan cycle and link to hakkas, the library, and the control channel's
// clients. Clients send requests; the core applies each, broadcasts the resulting state to
// every client, the sender included, then answers the sender (§2). Transports only frame
// messages.
import type {Catalog} from './catalog.js';
import {isObject, type Json, type JsonObject} from './check.js';
import {checkDescription, describe, type Description} from './description.js';
import type {McuType} from './descriptors.js';
import {carrierOf, checkEnvironment, type Environment} from './environment.js';
import {LibraryError, type Library} from './library.js';
import type {LinkServer} from './link.js';
import type {Logger} from './log.js';
import {VERSION} from './package.js';
import type {Random} from './random.js';
import {Scanner, type Stats, type Turn} from './scan.js';
import {EMPTY_SCENE, scene} from './scene.js';
import {configure, fieldToJson, settings, SettingsError} from './settings.js';
import {ModelError, System, type Clock} from './system.js';

export const PROTOCOL = 1;
export const STATE_MS = 50;         // state at most this often (§3.5)
export const STATS_MS = 1000;       // stats at most this often (§3.6)

// A connected client: what the transport sends it.
export interface Client {
    send(message: object): void;
}

// A refused request; its message is the protocol's.
export class RequestError extends Error {}

interface Run {
    name: string;
    env: Environment;           // the running configuration, changes not persisted included
    system: System;
    scanner: Scanner;
    description: Description;
}

type Payload = Record<string, Json>;

export class Core {
    private run: Run | undefined;
    private names: string[] = [];
    private unsaved = false;
    private readonly clients = new Set<Client>();
    private dirty = new Map<string, Json>();
    private stateTimer: NodeJS.Timeout | undefined;
    private lastState = -Infinity;
    private statsTimer: NodeJS.Timeout | undefined;
    private sentStats: Stats = {parts: {}, channels: {}};
    private queue: Promise<void> = Promise.resolve();

    constructor(private readonly catalog: Catalog, private readonly library: Library,
                private readonly link: LinkServer, private readonly clock: Clock,
                private readonly random: Random, private readonly log: Logger,
                private readonly mcu?: McuType) {
        link.events.state = state => this.broadcast({link: {state}});
        link.events.notice = (level, message) => this.broadcast({notice: {level, message}});
    }

    // Runs the startup environment, if it can be run (spec §5.1).
    async start(name: string): Promise<void> {
        this.names = await this.library.list();
        const {run} = await this.open(name);
        if (run !== undefined)
            this.launch(run);
        this.statsTimer = setInterval(() => this.sendStats(), STATS_MS);
    }

    stop(): void {
        this.run?.scanner.stop();
        clearInterval(this.statsTimer);
        clearTimeout(this.stateTimer);
    }

    get running(): string | undefined {
        return this.run?.name;
    }

    // A new client gets the whole state (§3).
    connect(client: Client): void {
        this.clients.add(client);
        client.send({hello: {protocol: PROTOCOL, emulator: 'colibri', version: VERSION, features: []}});
        client.send(this.libraryMessage());
        client.send(this.settingsMessage());
        client.send({scene: this.run === undefined ? EMPTY_SCENE : scene(this.run.system, this.catalog)});
        client.send({state: {values: this.run?.system.state() ?? {}}});
        client.send({stats: this.run?.scanner.stats() ?? {parts: {}, channels: {}}});
        client.send({link: {state: this.link.state}});
    }

    disconnect(client: Client): void {
        this.clients.delete(client);
    }

    // A message from a client. Requests are handled one at a time, in arrival order (§2).
    receive(client: Client, text: string): Promise<void> {
        this.queue = this.queue.then(() => this.handle(client, text)).catch(e => {
            this.log.error(`control: ${(e as Error).stack ?? e}`);
        });
        return this.queue;
    }

    private async handle(client: Client, text: string): Promise<void> {
        let message: unknown;
        try {
            message = JSON.parse(text);
        } catch {
            message = undefined;
        }
        if (!isObject(message as Json) || Object.keys(message as object).length !== 1)
            return client.send({error: {message: 'Invalid message'}});
        const [request, body] = Object.entries(message as JsonObject)[0]!;
        const p: Payload = isObject(body) ? body : {};
        const id = typeof p.id === 'number' || typeof p.id === 'string' ? {id: p.id} : {};
        if (!isObject(body))
            return client.send({error: {...id, request, message: 'Invalid message'}});
        try {
            const result = await this.apply(request, p);
            this.flushState();
            client.send({ok: {...id, ...result}});
        } catch (e) {
            if (!(e instanceof RequestError || e instanceof ModelError || e instanceof SettingsError
                || e instanceof LibraryError)) {
                this.log.error(`control: ${request}: ${(e as Error).message}`);
                if (request === 'persist')
                    this.broadcast({notice: {level: 'error', message: `Persist failed: ${(e as Error).message}`}});
            }
            client.send({error: {...id, request, message: (e as Error).message}});
        }
    }

    private async apply(request: string, p: Payload): Promise<JsonObject> {
        switch (request) {
        case 'set':
            return this.set(p);
        case 'configure':
            return this.configure(p);
        case 'persist':
            return this.persist();
        case 'create':
            await this.library.create(name(p, 'name'), p.from === undefined ? undefined : name(p, 'from'));
            await this.libraryChanged();
            return {};
        case 'rename':
            return this.rename(name(p, 'name'), name(p, 'to'));
        case 'delete': {
            const n = name(p, 'name');
            if (n === this.run?.name)
                throw new RequestError(`Environment running: ${n}`);
            await this.library.delete(n);
            await this.libraryChanged();
            return {};
        }
        case 'select':
            return this.select(name(p, 'name'), p.discard === true);
        case 'resetstats':
            this.running_().scanner.resetStats();
            this.sentStats = this.run!.scanner.stats();
            this.broadcast({stats: this.sentStats});
            return {};
        default:
            throw new RequestError(`Unknown request: ${request}`);
        }
    }

    private running_(): Run {
        if (this.run === undefined)
            throw new RequestError('No environment');
        return this.run;
    }

    // §4.1: one key, or several all or none.
    private set(p: Payload): JsonObject {
        const run = this.running_();
        let values: Record<string, Json>;
        if (p.values !== undefined) {
            if (!isObject(p.values))
                throw new RequestError('Invalid field: values');
            values = p.values;
        } else {
            if (typeof p.key !== 'string')
                throw new RequestError('Invalid field: key');
            if (p.value === undefined)
                throw new RequestError('Invalid field: value');
            values = {[p.key]: p.value};
        }
        run.system.setMany(values);
        const state = run.system.state();
        for (const key of Object.keys(values))
            this.dirty.set(key, state[key]!);
        this.markUnsaved();
        return {};
    }

    // §4.2: settings, all or none; a `restart` field's change is a power cycle.
    private configure(p: Payload): JsonObject {
        const run = this.running_();
        if (!isObject(p.values))
            throw new RequestError('Invalid field: values');
        const {env, restart} = configure(run.env, this.catalog, p.values);
        if (JSON.stringify(env) === JSON.stringify(run.env))
            return {};
        if (restart) {
            this.powerCycle(run.name, env);
        } else {
            run.env = env;
            run.scanner.setCycleMs(env.cycleMs);
            run.scanner.setQueueSize(env.queueSize);
            for (const s of run.system.comm) {
                const c = env.comm.find(x => x.slot === s.slot)!;
                s.params = Object.fromEntries(s.def.params.filter(k => c[k] !== undefined).map(k => [k, c[k]!]));
            }
            run.description = this.describe(run.system);
            this.link.updateDescription(run.description);
            this.broadcast(this.settingsMessage());
        }
        this.markUnsaved();
        return {};
    }

    // §4.3: the whole running environment to its file.
    private async persist(): Promise<JsonObject> {
        const run = this.running_();
        await this.library.write(run.name, {...run.env, inputs: run.system.toEnvironment().inputs});
        this.unsaved = false;
        this.broadcast(this.libraryMessage());
        return {};
    }

    private async rename(from: string, to: string): Promise<JsonObject> {
        await this.library.rename(from, to);
        if (from === this.run?.name && from !== to)
            this.powerCycle(to, this.run.env);     // the name the runtime knows it by
        await this.libraryChanged();
        return {};
    }

    private async select(n: string, discard: boolean): Promise<JsonObject> {
        if (this.unsaved && !discard)
            throw new RequestError('Unsaved changes');
        const {run, errors} = await this.open(n);
        if (run === undefined)
            throw new RequestError(`Cannot be run: ${n}: ${errors[0] ?? 'unknown problem'}`);
        this.unsaved = false;
        this.launch(run);
        this.broadcast(this.libraryMessage());
        return {};
    }

    // Reads an environment and builds its system; errors say why it cannot be run.
    private async open(n: string): Promise<{run?: Run, errors: string[]}> {
        const {env, errors} = await openEnvironment(this.library, n, this.catalog, this.log);
        if (env === undefined)
            return {errors};
        return this.build(n, env, this.log.warn);
    }

    private build(n: string, env: Environment, warn: (m: string) => void): {run?: Run, errors: string[]} {
        const {system, warnings} = System.build(n, env, this.catalog, this.clock, this.random);
        for (const w of warnings)
            warn(`environment ${n}: ${w}`);
        const description = this.describe(system, warn, n);
        const problems = checkDescription(description);
        if (problems.length > 0) {
            for (const p of problems)
                this.log.error(`environment ${n}: description: ${p}`);
            this.log.error(`environment ${n} cannot be run`);
            return {errors: problems.map(p => `description: ${p}`)};
        }
        this.log.fine(`environment ${n}: ${description.outPorts.length} sensor ports, ` +
            `${description.inPorts.length} actuator ports`);
        const scanner = new Scanner(system, env, this.clock, this.log, t => this.turn(t));
        return {run: {name: n, env, system, scanner, description}, errors: []};
    }

    // The description, with the MCU module if the carrier takes it (spec §5.4); `warn`, if given,
    // is told when it does not.
    private describe(system: System, warn?: (m: string) => void, n?: string): Description {
        const takes = this.mcu !== undefined && system.carrier.mcu?.types.includes(this.mcu.type) === true;
        if (this.mcu !== undefined && !takes)
            warn?.(`environment ${n}: carrier ${system.carrier.type} takes no MCU module ${this.mcu.type}: ` +
                'not described');
        return describe(system, takes ? this.mcu : undefined);
    }

    // A power cycle (spec §5.1): the system built anew; inputs keep their sources and noise where
    // their keys still exist; queues emptied and outputs at their defaults.
    private powerCycle(n: string, env: Environment): void {
        const inputs = this.run!.system.toEnvironment().inputs;
        const {run, errors} = this.build(n, {...env, inputs}, m => this.log.fine(`${m} (power cycle)`));
        if (run === undefined)
            throw new RequestError(`Cannot be run: ${n}: ${errors[0]}`);
        run.env = env;
        this.launch(run);
    }

    // Runs a system in place of the one running: the link serves it (a power cycle when hakkas
    // had the other one), and every client gets the new structure and state.
    private launch(run: Run): void {
        this.run?.scanner.stop();
        this.run = run;
        this.link.serve({system: run.system, scanner: run.scanner, description: run.description});
        run.scanner.start();
        this.dirty.clear();
        this.sentStats = run.scanner.stats();
        this.broadcast(this.settingsMessage());
        this.broadcast({scene: scene(run.system, this.catalog)});
        this.broadcast({state: {values: run.system.state()}});
        this.broadcast({stats: this.sentStats});
        this.broadcast({link: {state: this.link.state}});
    }

    // A scan turn: its changes go to hakkas now, and to the clients with the next `state`.
    private turn(t: Turn): void {
        for (const ch of [...t.changed, ...t.applied])
            this.dirty.set(ch.key, ch.value);
        if (this.dirty.size > 0)
            this.scheduleState();
        this.link.turn(t);
    }

    private scheduleState(): void {
        if (this.stateTimer !== undefined)
            return;
        const wait = Math.max(0, this.lastState + STATE_MS - this.clock());
        this.stateTimer = setTimeout(() => this.flushState(), wait);
    }

    // The keys that changed since the last `state`, now.
    private flushState(): void {
        clearTimeout(this.stateTimer);
        this.stateTimer = undefined;
        if (this.dirty.size === 0)
            return;
        const values = Object.fromEntries(this.dirty);
        this.dirty.clear();
        this.lastState = this.clock();
        this.broadcast({state: {values}});
    }

    // The counters that changed since the last `stats`.
    private sendStats(): void {
        if (this.run === undefined)
            return;
        const now = this.run.scanner.stats();
        const changed: Stats = {parts: {}, channels: {}};
        let any = false;
        for (const kind of ['parts', 'channels'] as const)
            for (const [k, v] of Object.entries(now[kind]))
                if (JSON.stringify(v) !== JSON.stringify(this.sentStats[kind][k])) {
                    (changed[kind] as Record<string, unknown>)[k] = v;
                    any = true;
                }
        this.sentStats = now;
        if (any)
            this.broadcast({stats: changed});
    }

    private markUnsaved(): void {
        if (this.unsaved)
            return;
        this.unsaved = true;
        this.broadcast(this.libraryMessage());
    }

    private async libraryChanged(): Promise<void> {
        this.names = await this.library.list();
        this.broadcast(this.libraryMessage());
    }

    private libraryMessage(): object {
        return {library: {environments: this.names.map(n => ({name: n})), running: this.run?.name ?? null,
            unsaved: this.unsaved}};
    }

    private settingsMessage(): object {
        if (this.run === undefined)
            return {settings: {fields: [], values: {}}};
        const s = settings(this.run.env, this.catalog);
        return {settings: {fields: s.fields.map(fieldToJson), values: s.values}};
    }

    private broadcast(message: object): void {
        for (const c of this.clients)
            c.send(message);
    }
}

function name(p: Payload, key: string): string {
    const v = p[key];
    if (typeof v !== 'string')
        throw new RequestError(`Invalid field: ${key}`);
    return v;
}

// Reads and checks an environment to run (spec §5): problems are logged, and one that cannot be
// run comes back with its errors; one that can becomes the last one run.
export async function openEnvironment(library: Library, name: string, catalog: Catalog,
                                      log: Logger): Promise<{env?: Environment, errors: string[]}> {
    const read = await library.read(name);
    const checked = read.env === undefined ? {errors: [], warnings: []} : checkEnvironment(read.env, catalog);
    for (const w of [...read.warnings, ...checked.warnings])
        log.warn(`environment ${name}: ${w}`);
    const errors = [...read.errors, ...checked.errors];
    for (const e of errors)
        log.error(`environment ${name}: ${e}`);
    if (read.env === undefined || errors.length > 0) {
        log.error(`environment ${name} cannot be run`);
        return {errors};
    }
    const env = read.env;
    log.info(`environment ${name}: carrier ${carrierOf(env, catalog)!.type}, ` +
        `modules ${env.modules.length}, cycle ${env.cycleMs} ms`);
    await library.setLastRun(name);
    return {env, errors: []};
}
