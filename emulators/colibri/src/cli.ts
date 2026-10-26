// The command line (spec §3.1).
import {parseArgs} from 'node:util';
import path from 'node:path';
import {TYPE_NAME} from './descriptors.js';
import {isLevel, type Level} from './log.js';
import {isEnvironmentName} from './names.js';

export const USAGE = `usage: colibri-emulator [options] [<environment>]

  --library <dir>          the environment library (default $XDG_DATA_HOME/colibri-emulator)
  --plugins <dir>          a further plugin directory, after the built-in one (repeatable)
  --socket <path>          the environment link's socket, hakkas' environment.socket
                           (default $XDG_RUNTIME_DIR/colibri-emulator/environment.sock)
  --control-socket <path>  the control channel's socket (default …/control.sock)
  --http <port>            the UI's HTTP port on localhost, 0 for any free one (default 8090)
  --mcu <type>             the MCU module in the carrier's MCU slot (default colibri_mcu6)
  --pictures <dir>         puts the boards' own picture copies there, hakkas' environment.pictures
  --seed <n>               seeds the noise, for a reproducible run (0 to 4294967295)
  --log-level <level>      fine, info, warn or error (default info)
  -h, --help               this text
  --version                the emulator's version
`;

export const DEFAULT_HTTP_PORT = 8090;
export const DEFAULT_MCU = 'colibri_mcu6';

// The longest path a Unix socket address holds on Linux (sun_path less its NUL).
export const SOCKET_PATH_MAX = 107;

export interface Options {
    library: string;
    plugins: string[];        // the --plugins directories, in order (the built-in one is not here)
    socket: string;
    controlSocket: string;
    httpPort: number;
    mcu: string;              // an MCU module type, checked against the catalog at startup
    pictures: string | undefined; // where the picture copies go; undefined: none described
    seed: number | undefined; // undefined: a random seed
    logLevel: Level;
    environment: string | undefined;
}

export type Command =
    | {kind: 'run', options: Options}
    | {kind: 'help'}
    | {kind: 'version'}
    | {kind: 'error', message: string};

// What the defaults are made from, so tests need not change the real environment.
export interface Host {
    env: Record<string, string | undefined>;
    uid: number;
    tmpdir: string;
}

export function parseCommandLine(argv: string[], host: Host): Command {
    let parsed;
    try {
        parsed = parseArgs({
            args: argv,
            allowPositionals: true,
            strict: true,
            options: {
                'library': {type: 'string'},
                'plugins': {type: 'string', multiple: true},
                'socket': {type: 'string'},
                'control-socket': {type: 'string'},
                'http': {type: 'string'},
                'mcu': {type: 'string'},
                'pictures': {type: 'string'},
                'seed': {type: 'string'},
                'log-level': {type: 'string'},
                'help': {type: 'boolean', short: 'h'},
                'version': {type: 'boolean'},
            },
        });
    } catch (e) {
        return {kind: 'error', message: (e as Error).message};
    }
    const v = parsed.values;
    if (v.help)
        return {kind: 'help'};
    if (v.version)
        return {kind: 'version'};

    if (parsed.positionals.length > 1)
        return {kind: 'error', message: 'expected at most one environment'};
    const environment = parsed.positionals[0];
    if (environment !== undefined && !isEnvironmentName(environment))
        return {kind: 'error', message: `invalid environment name: ${environment}`};

    const library = v.library ?? defaultLibrary(host);
    if (library === undefined)
        return {kind: 'error', message: 'no library directory: neither XDG_DATA_HOME nor HOME is set; use --library'};

    for (const dir of [library, ...(v.plugins ?? []), ...(v.pictures !== undefined ? [v.pictures] : [])])
        if (dir === '')
            return {kind: 'error', message: 'empty directory name'};

    const runtime = runtimeDir(host);
    const socket = v.socket ?? path.join(runtime, 'environment.sock');
    const controlSocket = v['control-socket'] ?? path.join(runtime, 'control.sock');
    for (const p of [socket, controlSocket]) {
        if (p === '')
            return {kind: 'error', message: 'empty socket path'};
        if (Buffer.byteLength(p) > SOCKET_PATH_MAX)
            return {kind: 'error', message: `socket path longer than ${SOCKET_PATH_MAX} bytes: ${p}`};
    }
    if (path.resolve(socket) === path.resolve(controlSocket))
        return {kind: 'error', message: `the environment and control sockets are the same: ${socket}`};

    const httpPort = v.http === undefined ? DEFAULT_HTTP_PORT : integer(v.http, 0, 65535);
    if (httpPort === undefined)
        return {kind: 'error', message: `invalid --http port: ${v.http}`};

    const mcu = v.mcu ?? DEFAULT_MCU;
    if (!TYPE_NAME.test(mcu))
        return {kind: 'error', message: `invalid --mcu type: ${mcu}`};

    const seed = v.seed === undefined ? undefined : integer(v.seed, 0, 0xffffffff);
    if (v.seed !== undefined && seed === undefined)
        return {kind: 'error', message: `invalid --seed: ${v.seed}`};

    const logLevel = v['log-level'] ?? 'info';
    if (!isLevel(logLevel))
        return {kind: 'error', message: `invalid --log-level: ${logLevel}`};

    return {
        kind: 'run',
        options: {library, plugins: v.plugins ?? [], socket, controlSocket, httpPort, mcu,
            pictures: v.pictures, seed, logLevel, environment},
    };
}

function defaultLibrary(host: Host): string | undefined {
    const data = host.env.XDG_DATA_HOME || (host.env.HOME ? path.join(host.env.HOME, '.local', 'share') : undefined);
    return data === undefined ? undefined : path.join(data, 'colibri-emulator');
}

// $XDG_RUNTIME_DIR/colibri-emulator, or a per-user directory in the temporary directory when the
// session has no runtime directory.
function runtimeDir(host: Host): string {
    return host.env.XDG_RUNTIME_DIR
        ? path.join(host.env.XDG_RUNTIME_DIR, 'colibri-emulator')
        : path.join(host.tmpdir, `colibri-emulator-${host.uid}`);
}

function integer(s: string, min: number, max: number): number | undefined {
    if (!/^[0-9]+$/.test(s))
        return undefined;
    const n = Number(s);
    return n >= min && n <= max ? n : undefined;
}
