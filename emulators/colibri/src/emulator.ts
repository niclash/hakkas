// The emulator's run: from startup to a stop request.
import {loadCatalog, publishCopies} from './catalog.js';
import type {Options} from './cli.js';
import {Core} from './core.js';
import {Library} from './library.js';
import {LinkServer} from './link.js';
import type {Logger} from './log.js';
import {BUILTIN_DATA, UI_DIST, VERSION} from './package.js';
import {randomSeed, seededRandom} from './random.js';
import {ControlSocket, HttpServer} from './transports.js';

// Runs the emulator until `stop` is aborted (SIGINT/SIGTERM, or a test). A failure that keeps
// it from starting is thrown.
export async function runEmulator(options: Options, log: Logger, stop: AbortSignal): Promise<void> {
    log.info(`colibri-emulator ${VERSION}: library ${options.library}`);
    log.fine(`plugin directories: ${[BUILTIN_DATA, ...options.plugins].join(', ')}`);
    const catalog = await loadCatalog([BUILTIN_DATA, ...options.plugins], log);
    const mcu = catalog.mcu.get(options.mcu)?.def;
    if (mcu === undefined)
        throw new Error(`unknown MCU module type: ${options.mcu}`);
    await publishCopies(catalog, options.pictures, log);
    const library = new Library(options.library);
    await library.init();
    const name = await library.startup(options.environment);
    const seed = options.seed ?? randomSeed();
    log.fine(`noise seed ${seed}`);
    const link = new LinkServer(options.socket, log);
    const core = new Core(catalog, library, link, () => performance.now(), seededRandom(seed), log, mcu);
    const control = new ControlSocket(options.controlSocket, core, log);
    const http = new HttpServer(options.httpPort, core, catalog, UI_DIST, log);
    try {
        await core.start(name);
        await link.listen();
        await control.listen();
        await http.listen();
        if (!stop.aborted)
            await new Promise<void>(resolve => stop.addEventListener('abort', () => resolve(), {once: true}));
    } finally {
        await http.close();
        await control.close();
        core.stop();
        await link.close();
    }
    log.info('stopped');
}
