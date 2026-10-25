// The emulator's run: from startup to a stop request.
import type {Options} from './cli.js';
import type {Logger} from './log.js';
import {BUILTIN_DATA, VERSION} from './package.js';

// Runs the emulator until `stop` is aborted (SIGINT/SIGTERM, or a test). A failure that keeps
// it from starting is thrown.
export async function runEmulator(options: Options, log: Logger, stop: AbortSignal): Promise<void> {
    log.info(`colibri-emulator ${VERSION}: library ${options.library}`);
    log.fine(`plugin directories: ${[BUILTIN_DATA, ...options.plugins].join(', ')}`);
    log.fine(`environment socket ${options.socket}, control socket ${options.controlSocket}, ` +
        `HTTP port ${options.httpPort}`);
    if (stop.aborted)
        return;
    // Nothing listens yet (the sockets come with plan tasks E7 and E10), so a timer keeps the
    // process alive until the stop request.
    const alive = setInterval(() => {}, 1 << 30);
    try {
        await new Promise<void>(resolve => stop.addEventListener('abort', () => resolve(), {once: true}));
    } finally {
        clearInterval(alive);
    }
    log.info('stopped');
}
