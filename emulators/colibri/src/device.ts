// Copying an environment from a device (spec §5.3, plan task E16): the emulator's server — not
// the browser, so there is no cross-origin access — logs in to the device's hakkas over its wire
// protocol (02 §2.3), asks for its environment description (02 §3.7) and makes an environment
// file of it: the device's twin.
import {createHash, createHmac, pbkdf2, randomBytes, timingSafeEqual} from 'node:crypto';
import {promisify} from 'node:util';
import WebSocket from 'ws';
import type {Catalog} from './catalog.js';
import {Checker, isObject, type Json, type JsonObject} from './check.js';
import {COMM_PARAMS, readCarrier, type CarrierType, type McuType} from './descriptors.js';
import {newEnvironment, type CommConfig, type Environment} from './environment.js';

// The whole copy's time (spec §5.3).
export const COPY_TIMEOUT_MS = 15_000;

// A refused copy; its message is the answer's (spec §5.3).
export class CopyError extends Error {}

// The device's WebSocket and the Origin a browser on its page would send (02 §0): the port after
// the web server's, ws:// for http:// and wss:// for https://.
export function deviceEndpoint(address: string): {ws: string, origin: string} {
    let url: URL;
    try {
        url = new URL(address);
    } catch {
        throw new CopyError(`Invalid address: ${address}`);
    }
    if ((url.protocol !== 'http:' && url.protocol !== 'https:') || url.hostname === '' || url.username !== ''
        || url.password !== '')
        throw new CopyError(`Invalid address: ${address}`);
    const port = Number(url.port || (url.protocol === 'https:' ? 443 : 80));
    if (port >= 65535)
        throw new CopyError(`Invalid address: ${address}`);
    const scheme = url.protocol === 'https:' ? 'wss:' : 'ws:';
    // (An IPv6 address's hostname has its brackets already.)
    return {ws: `${scheme}//${url.hostname}:${port + 1}/`, origin: url.origin};
}

const pbkdf2Async = promisify(pbkdf2);
const hmac = (key: Buffer, text: string) => createHmac('sha256', key).update(text, 'utf8').digest();

// The login proof and the signature the server must answer with (02 §2.3, SCRAM-SHA-256).
export async function loginProof(user: string, password: string,
                                 challenge: {salt: string, iterations: number, nonce: string}):
    Promise<{proof: string, serverSignature: Buffer}> {
    const salted = await pbkdf2Async(Buffer.from(password, 'utf8'), Buffer.from(challenge.salt, 'base64'),
        challenge.iterations, 32, 'sha256');
    const clientKey = hmac(salted, 'Client Key');
    const storedKey = createHash('sha256').update(clientKey).digest();
    const serverKey = hmac(salted, 'Server Key');
    const authMessage = `${user},${challenge.nonce},${challenge.salt},${challenge.iterations}`;
    const signature = hmac(storedKey, authMessage);
    const proof = Buffer.alloc(32);
    for (let i = 0; i < 32; i++)
        proof[i] = clientKey[i]! ^ signature[i]!;
    salted.fill(0);
    clientKey.fill(0);
    return {proof: proof.toString('base64'), serverSignature: hmac(serverKey, authMessage)};
}

// Logs in, asks for the description, logs out (spec §5.3 steps 2–4). The password is used for
// the proof only: never stored, logged or passed on.
export async function fetchDescription(address: string, user: string, password: string,
                                       timeoutMs = COPY_TIMEOUT_MS): Promise<JsonObject> {
    const {ws: url, origin} = deviceEndpoint(address);
    const ws = new WebSocket(url, {origin, handshakeTimeout: timeoutMs});
    const received: JsonObject[] = [];
    let wake: (() => void) | undefined;
    let closed = false;
    ws.on('message', raw => {
        try {
            const m = JSON.parse(raw.toString()) as Json;
            if (isObject(m))
                received.push(m);
        } catch {
            // Not a message of the protocol: ignored.
        }
        wake?.();
    });
    ws.on('close', () => {
        closed = true;
        wake?.();
    });
    ws.on('error', () => {});
    const deadline = Date.now() + timeoutMs;
    const timeout = () => {
        if (Date.now() >= deadline)
            throw new CopyError('Timeout');
    };
    // The next message of protocol:command (or an error), waiting for it.
    const next = async (protocol: string, command: string): Promise<JsonObject> => {
        for (;;) {
            const i = received.findIndex(m => (m.protocol === protocol && m.command === command)
                || (m.protocol === 'network' && m.command === 'error'));
            if (i >= 0)
                return received.splice(0, i + 1)[i]!;
            if (closed)
                throw new CopyError(`Cannot connect: ${address}`);
            timeout();
            await new Promise<void>(resolve => {
                const timer = setTimeout(resolve, Math.max(0, deadline - Date.now()));
                wake = () => {
                    clearTimeout(timer);
                    resolve();
                };
            });
        }
    };
    let secret = '';
    const send = (protocol: string, command: string, payload: JsonObject) =>
        ws.send(JSON.stringify({protocol, command, payload, secret}));
    try {
        await new Promise<void>((resolve, reject) => {
            ws.once('open', () => resolve());
            ws.once('error', () =>
                reject(new CopyError(Date.now() >= deadline ? 'Timeout' : `Cannot connect: ${address}`)));
            ws.once('close', () => reject(new CopyError(`Cannot connect: ${address}`)));
            setTimeout(() => reject(new CopyError('Timeout')), Math.max(0, deadline - Date.now())).unref();
        });
        send('auth', 'challenge', {user, cnonce: randomBytes(18).toString('base64')});
        const challenge = await next('auth', 'challenge');
        const c = challenge.payload as JsonObject | undefined;
        if (challenge.command !== 'challenge' || !isObject(c ?? null) || typeof c!.salt !== 'string'
            || typeof c!.iterations !== 'number' || typeof c!.nonce !== 'string')
            throw new CopyError('Login failed');
        const {proof, serverSignature} = await loginProof(user, password,
            {salt: c!.salt, iterations: c!.iterations, nonce: c!.nonce});
        timeout();
        send('auth', 'login', {user, nonce: c!.nonce, proof});
        const login = await next('auth', 'login');
        const l = login.payload as JsonObject | undefined;
        const signature = typeof l?.server_signature === 'string'
            ? Buffer.from(l.server_signature, 'base64') : Buffer.alloc(0);
        // The server proves it knows the password too (02 §2.3).
        if (login.command !== 'login' || typeof l?.secret !== 'string' || signature.length !== serverSignature.length
            || !timingSafeEqual(signature, serverSignature))
            throw new CopyError('Login failed');
        secret = l.secret;
        send('environment', 'describe', {});
        const described = await next('environment', 'description');
        if (described.command !== 'description')
            throw new CopyError((described.payload as JsonObject | undefined)?.message === 'No environment'
                ? 'No environment' : `Cannot copy: ${String((described.payload as JsonObject | undefined)?.message)}`);
        if (!isObject(described.payload ?? null))
            throw new CopyError('Not a Colibri environment');
        send('auth', 'logout', {});
        return described.payload as JsonObject;
    } finally {
        ws.close();
    }
}

// ---- The environment ----------------------------------------------------------------------

const str = (v: Json | undefined): string | undefined => typeof v === 'string' ? v : undefined;
const int = (v: Json | undefined): number | undefined => typeof v === 'number' && Number.isInteger(v) ? v : undefined;

// The environment a description makes (spec §5.3 step 5): the carrier (its definition from the
// catalog, or one made from the description), the communication modules with their parameters,
// a module per changeable slot with its serial, inputs at their defaults. What the catalog lacks
// is a warning. `mcu` is the MCU module the emulator names (--mcu).
export function environmentFrom(d: JsonObject, catalog: Catalog, mcu?: McuType): {env: Environment, warnings: string[]} {
    const warnings: string[] = [];
    const c = d.carrier;
    if (!isObject(c ?? null))
        throw new CopyError('Not a Colibri environment');
    const carrierJson = c as JsonObject;
    const type = str(carrierJson.type);
    const modules = Array.isArray(d.modules) ? d.modules.filter(isObject) as JsonObject[] : [];
    const env = newEnvironment();

    let carrier = type !== undefined ? catalog.carriers.get(type)?.def : undefined;
    if (carrier !== undefined) {
        env.carrier = carrier.type;
    } else {
        carrier = carrierFrom(carrierJson, modules, catalog, warnings);
        env.carrier = carrier;
        warnings.push(`carrier type ${type ?? '(none)'} is not in the catalog: its definition was made from the ` +
            'description and is kept in the environment');
    }

    const builtin = new Set(carrier.builtin.map(b => b.slot));
    for (const m of modules) {
        const slot = int(m.slot), mt = str(m.type);
        if (slot === undefined || builtin.has(slot) || m.builtin === true)
            continue;
        if (mt === undefined || !catalog.modules.has(mt)) {
            warnings.push(`slot ${slot}: module type ${mt ?? '(none)'} is not in the catalog: left empty`);
            continue;
        }
        env.modules.push({slot, type: mt, ...(str(m.serial) !== undefined && {serial: str(m.serial)})});
    }

    for (const s of Array.isArray(carrierJson.comm) ? carrierJson.comm.filter(isObject) as JsonObject[] : []) {
        const slot = int(s.slot), ct = str(s.type);
        if (slot === undefined)
            continue;
        if (ct === undefined || !catalog.comm.has(ct)) {
            warnings.push(`communication slot ${slot}: type ${ct ?? '(none)'} is not in the catalog: left empty`);
            continue;
        }
        if (!carrier.comm.find(x => x.slot === slot)?.types.includes(ct)) {
            warnings.push(`communication slot ${slot}: the carrier does not take ${ct}: left empty`);
            continue;
        }
        const comm: CommConfig = {slot, type: ct};
        for (const p of COMM_PARAMS)
            if (isObject(s[p] ?? null))
                comm[p] = s[p] as JsonObject;
        env.comm.push(comm);
    }

    const deviceMcu = isObject(carrierJson.mcu ?? null) ? str((carrierJson.mcu as JsonObject).type) : undefined;
    if (deviceMcu !== undefined && deviceMcu !== mcu?.type)
        warnings.push(`the device's MCU module is ${deviceMcu}; the emulator names the one --mcu gives` +
            (mcu !== undefined ? ` (${mcu.type})` : ''));
    return {env, warnings};
}

// A carrier definition made from a description, for a carrier type the catalog lacks: its type,
// names, slots, built-in slots (each with the catalog's module type of the device's module there,
// else `carrier`), power, communication slots and MCU slot.
function carrierFrom(c: JsonObject, modules: JsonObject[], catalog: Catalog, warnings: string[]): CarrierType {
    const raw: JsonObject = {type: str(c.type) ?? 'device_carrier'};
    for (const k of ['name', 'vendor', 'revision', 'documentation'])
        if (str(c[k]) !== undefined)
            raw[k] = c[k]!;
    raw.slots = int(c.slots) ?? 0;
    raw.builtin = (Array.isArray(c.builtin) ? c.builtin.filter(isObject) as JsonObject[] : []).map(b => {
        const slot = int(b.slot) ?? 0;
        const there = str(modules.find(m => int(m.slot) === slot)?.type);
        const type = there !== undefined && catalog.modules.has(there) ? there : 'carrier';
        if (there !== undefined && type !== there)
            warnings.push(`built-in slot ${slot}: module type ${there} is not in the catalog: carrier used`);
        return {slot, type};
    });
    if (isObject(c.power ?? null))
        raw.power = c.power!;
    raw.comm = (Array.isArray(c.comm) ? c.comm.filter(isObject) as JsonObject[] : []).flatMap(s => {
        const slot = int(s.slot), t = str(s.type);
        const types = t !== undefined && catalog.comm.has(t) ? [t] : [...catalog.comm.keys()];
        return slot === undefined ? [] : [{slot, types}];
    });
    const mcu = isObject(c.mcu ?? null) ? str((c.mcu as JsonObject).type) : undefined;
    if (mcu !== undefined && catalog.mcu.has(mcu))
        raw.mcu = {types: [mcu]};
    const check = (o: JsonObject) => {
        const ch = new Checker();
        return {def: readCarrier(ch, o, String(o.type)), errors: ch.errors};
    };
    let r = check(raw);
    if (r.def === undefined && raw.power !== undefined) {
        // Its power is view only, and may be of any form: without it, rather than no copy.
        delete raw.power;
        warnings.push('the carrier\'s power is not of the emulator\'s form: left out');
        r = check(raw);
    }
    if (r.def === undefined)
        throw new CopyError(`Not a Colibri environment: ${r.errors[0] ?? 'its carrier'}`);
    return r.def;
}
