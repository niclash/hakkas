// The catalog: every module, carrier, communication module and MCU module type, loaded from the plugin
// directories (spec §7): the built-in one, then each --plugins directory, in order.
import {access, readdir, readFile} from 'node:fs/promises';
import path from 'node:path';
import {Checker, type Json} from './check.js';
import {readCarrier, readComm, readMcu, readModule, type CarrierType, type CommType, type McuType,
    type ModuleType} from './descriptors.js';
import type {Logger} from './log.js';

export interface Entry<T> {
    def: T;
    dir: string;    // the descriptor's directory, where its pictures are
}

export interface Catalog {
    modules: Map<string, Entry<ModuleType>>;
    carriers: Map<string, Entry<CarrierType>>;
    comm: Map<string, Entry<CommType>>;
    mcu: Map<string, Entry<McuType>>;
}

type Read = (c: Checker, v: Json, dirName: string) => {type: string} | undefined;

const KINDS: {subdir: string, file: string, what: string, read: Read}[] = [
    {subdir: 'modules', file: 'module.json', what: 'module', read: readModule},
    {subdir: 'carriers', file: 'carrier.json', what: 'carrier', read: readCarrier},
    {subdir: 'comm', file: 'comm.json', what: 'communication module', read: readComm},
    {subdir: 'mcu', file: 'mcu.json', what: 'MCU module', read: readMcu},
];

// A descriptor that cannot be read or is invalid is skipped, with an error naming the file; a type
// defined again in a later directory replaces the earlier one, with a warning. Nothing here
// refuses startup.
export async function loadCatalog(dirs: string[], log: Logger): Promise<Catalog> {
    const catalog: Catalog = {modules: new Map(), carriers: new Map(), comm: new Map(), mcu: new Map()};
    const maps = [catalog.modules, catalog.carriers, catalog.comm, catalog.mcu] as Map<string, Entry<unknown>>[];
    for (const dir of dirs) {
        let found = false;
        for (const [k, kind] of KINDS.entries()) {
            const base = path.join(dir, kind.subdir);
            let names: string[];
            try {
                names = (await readdir(base, {withFileTypes: true}))
                    .filter(d => d.isDirectory()).map(d => d.name).sort();
            } catch (e) {
                if ((e as NodeJS.ErrnoException).code !== 'ENOENT')
                    log.error(`${base}: ${(e as Error).message}`);
                continue;
            }
            found = true;
            for (const name of names) {
                const typeDir = path.join(base, name);
                const file = path.join(typeDir, kind.file);
                const def = await readDescriptor(file, name, kind.read, log);
                if (def === undefined)
                    continue;
                await checkPicture(def as {faceplate?: string, picture?: string}, typeDir, file, log);
                const map = maps[k]!;
                const earlier = map.get(def.type);
                if (earlier !== undefined)
                    log.warn(`${file}: replaces the ${kind.what} type ${def.type} of ${earlier.dir}`);
                map.set(def.type, {def, dir: typeDir});
            }
        }
        if (!found)
            log.warn(`${dir}: no modules, carriers, comm or mcu directory`);
    }
    crossCheck(catalog, log);
    log.info(`catalog: module types ${catalog.modules.size}, carrier types ${catalog.carriers.size}, ` +
        `communication module types ${catalog.comm.size}, MCU module types ${catalog.mcu.size}`);
    return catalog;
}

async function readDescriptor<T extends {type: string}>(
    file: string, dirName: string, read: (c: Checker, v: Json, dirName: string) => T | undefined,
    log: Logger): Promise<T | undefined> {
    let json: Json;
    try {
        json = JSON.parse(await readFile(file, 'utf8'));
    } catch (e) {
        const err = e as NodeJS.ErrnoException;
        log.error(`${file}: ${err.code === 'ENOENT' ? 'missing' : err.message}: skipped`);
        return undefined;
    }
    const c = new Checker();
    const def = read(c, json, dirName);
    for (const w of c.warnings)
        log.warn(`${file}: ${w}`);
    for (const e of c.errors)
        log.error(`${file}: ${e}`);
    if (def === undefined)
        log.error(`${file}: skipped`);
    return def;
}

// A picture the descriptor names but its directory does not have is left out, with a warning
// (the UI then draws a labelled box, control protocol §3.4).
async function checkPicture(def: {faceplate?: string, picture?: string}, dir: string, file: string,
                            log: Logger): Promise<void> {
    for (const key of ['faceplate', 'picture'] as const) {
        const name = def[key];
        if (name === undefined)
            continue;
        try {
            await access(path.join(dir, name));
        } catch {
            log.warn(`${file}: ${key} ${name} not found, left out`);
            delete def[key];
        }
    }
}

// What a carrier names must be in the catalog: a carrier whose built-in module type is missing
// is skipped (its slot could not be filled); a communication or MCU type that is missing is left
// out of its list.
function crossCheck(catalog: Catalog, log: Logger): void {
    for (const [type, {def, dir}] of catalog.carriers) {
        const file = path.join(dir, 'carrier.json');
        const missing = def.builtin.filter(b => !catalog.modules.has(b.type));
        if (missing.length > 0) {
            for (const b of missing)
                log.error(`${file}: built-in slot ${b.slot}: unknown module type ${b.type}`);
            log.error(`${file}: skipped`);
            catalog.carriers.delete(type);
            continue;
        }
        for (const slot of def.comm)
            slot.types = slot.types.filter(t => {
                if (catalog.comm.has(t))
                    return true;
                log.warn(`${file}: communication slot ${slot.slot}: unknown communication module type ${t}, left out`);
                return false;
            });
        if (def.mcu !== undefined)
            def.mcu.types = def.mcu.types.filter(t => {
                if (catalog.mcu.has(t))
                    return true;
                log.warn(`${file}: unknown MCU module type ${t}, left out`);
                return false;
            });
    }
}
