// The environment library (spec §5.1): a directory with `environments/` (one `<name>.json` per
// environment), `bindings/` (the user's bindings files, which the emulator does not read) and
// `state.json` (which environment ran last). Every operation acts on the files at once.
import {mkdir, readdir, readFile, unlink} from 'node:fs/promises';
import path from 'node:path';
import {Checker, isObject, type Json} from './check.js';
import {newEnvironment, readEnvironment, writeEnvironment, type Environment} from './environment.js';
import {renameExclusive, writeFileAtomic} from './files.js';
import {isEnvironmentName} from './names.js';

// A refused library operation; its message is the control protocol's (§4.4).
export class LibraryError extends Error {}

export interface ReadResult {
    env?: Environment;      // undefined: the file is unusable, `errors` say why
    errors: string[];
    warnings: string[];
}

export class Library {
    readonly environments: string;
    readonly bindings: string;
    private readonly state: string;

    constructor(readonly dir: string) {
        this.environments = path.join(dir, 'environments');
        this.bindings = path.join(dir, 'bindings');
        this.state = path.join(dir, 'state.json');
    }

    // Creates the library's directories where they are missing.
    async init(): Promise<void> {
        await mkdir(this.environments, {recursive: true});
        await mkdir(this.bindings, {recursive: true});
    }

    // The environments' names, sorted. A file whose name is not an environment's name is not one.
    async list(): Promise<string[]> {
        const entries = await readdir(this.environments, {withFileTypes: true});
        return entries
            .filter(e => e.isFile() && e.name.endsWith('.json'))
            .map(e => e.name.slice(0, -'.json'.length))
            .filter(isEnvironmentName)
            .sort();
    }

    file(name: string): string {
        return path.join(this.environments, `${name}.json`);
    }

    async read(name: string): Promise<ReadResult> {
        checkName(name);
        let text: string;
        try {
            text = await readFile(this.file(name), 'utf8');
        } catch (e) {
            if ((e as NodeJS.ErrnoException).code === 'ENOENT')
                throw new LibraryError(`Unknown environment: ${name}`);
            throw e;
        }
        let json: Json;
        try {
            json = JSON.parse(text);
        } catch (e) {
            return {errors: [(e as Error).message], warnings: []};
        }
        const c = new Checker();
        const env = readEnvironment(c, json);
        return {...(env !== undefined && {env}), errors: c.errors, warnings: c.warnings};
    }

    // Writes an environment's file, replacing it (the control protocol's `persist`).
    async write(name: string, env: Environment): Promise<void> {
        checkName(name);
        await writeFileAtomic(this.file(name), writeEnvironment(env));
    }

    // A new environment with the defaults, or a copy of `from`'s file (its saved state).
    async create(name: string, from?: string): Promise<void> {
        checkName(name);
        if (from === undefined) {
            await this.exclusive(name, () => writeFileAtomic(this.file(name), writeEnvironment(newEnvironment()), true));
            return;
        }
        checkName(from);
        const text = await readFile(this.file(from), 'utf8').catch(e => {
            throw (e as NodeJS.ErrnoException).code === 'ENOENT' ? new LibraryError(`Unknown environment: ${from}`) : e;
        });
        await this.exclusive(name, () => writeFileAtomic(this.file(name), text, true));
    }

    async rename(name: string, to: string): Promise<void> {
        checkName(name);
        checkName(to);
        if (name === to)
            return;
        try {
            await renameExclusive(this.file(name), this.file(to));
        } catch (e) {
            const code = (e as NodeJS.ErrnoException).code;
            if (code === 'EEXIST')
                throw new LibraryError(`Environment exists: ${to}`);
            if (code === 'ENOENT')
                throw new LibraryError(`Unknown environment: ${name}`);
            throw e;
        }
        if (await this.lastRun() === name)
            await this.setLastRun(to);
    }

    async delete(name: string): Promise<void> {
        checkName(name);
        try {
            await unlink(this.file(name));
        } catch (e) {
            if ((e as NodeJS.ErrnoException).code === 'ENOENT')
                throw new LibraryError(`Unknown environment: ${name}`);
            throw e;
        }
    }

    // The environment that ran last, as `state.json` has it (undefined when it has none).
    async lastRun(): Promise<string | undefined> {
        try {
            const o = JSON.parse(await readFile(this.state, 'utf8')) as Json;
            return isObject(o) && typeof o.last === 'string' && isEnvironmentName(o.last) ? o.last : undefined;
        } catch {
            return undefined;
        }
    }

    async setLastRun(name: string): Promise<void> {
        await writeFileAtomic(this.state, JSON.stringify({last: name}) + '\n');
    }

    // The environment to run at startup (spec §5.1): the one named on the command line, else
    // the one that ran last, else the first of the library; an empty library gets `default`.
    async startup(named?: string): Promise<string> {
        const names = await this.list();
        if (named !== undefined) {
            if (!names.includes(named))
                throw new LibraryError(`Unknown environment: ${named}`);
            return named;
        }
        const last = await this.lastRun();
        if (last !== undefined && names.includes(last))
            return last;
        if (names.length > 0)
            return names[0]!;
        await this.create('default');
        return 'default';
    }

    private async exclusive(name: string, write: () => Promise<void>): Promise<void> {
        try {
            await write();
        } catch (e) {
            if ((e as NodeJS.ErrnoException).code === 'EEXIST')
                throw new LibraryError(`Environment exists: ${name}`);
            throw e;
        }
    }
}

function checkName(name: string): void {
    if (!isEnvironmentName(name))
        throw new LibraryError('Invalid name');
}
