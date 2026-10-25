// The emulator's run: from startup to a stop request.
import {loadCatalog, type Catalog} from './catalog.js';
import type {Options} from './cli.js';
import {checkDescription, describe} from './description.js';
import {carrierOf, checkEnvironment, type Environment} from './environment.js';
import {Library} from './library.js';
import {LinkServer} from './link.js';
import type {Logger} from './log.js';
import {BUILTIN_DATA, VERSION} from './package.js';
import {randomSeed, seededRandom} from './random.js';
import {Scanner} from './scan.js';
import {System} from './system.js';

// Runs the emulator until `stop` is aborted (SIGINT/SIGTERM, or a test). A failure that keeps
// it from starting is thrown.
export async function runEmulator(options: Options, log: Logger, stop: AbortSignal): Promise<void> {
    log.info(`colibri-emulator ${VERSION}: library ${options.library}`);
    log.fine(`plugin directories: ${[BUILTIN_DATA, ...options.plugins].join(', ')}`);
    log.fine(`environment socket ${options.socket}, control socket ${options.controlSocket}, ` +
        `HTTP port ${options.httpPort}`);
    const catalog = await loadCatalog([BUILTIN_DATA, ...options.plugins], log);
    const library = new Library(options.library);
    await library.init();
    const name = await library.startup(options.environment);
    const env = await openEnvironment(library, name, catalog, log);
    const seed = options.seed ?? randomSeed();
    log.fine(`noise seed ${seed}`);
    const random = seededRandom(seed);
    const link = new LinkServer(options.socket, log);
    await link.listen();
    const clock = () => performance.now();
    let scanner: Scanner | undefined;
    if (env !== undefined) {
        const {system, warnings} = System.build(name, env, catalog, clock, random);
        for (const w of warnings)
            log.warn(`environment ${name}: ${w}`);
        const description = describe(system);
        const problems = checkDescription(description);
        for (const p of problems)
            log.error(`environment ${name}: description: ${p}`);
        if (problems.length > 0) {
            log.error(`environment ${name} cannot be run`);
        } else {
            log.fine(`environment ${name}: ${description.outPorts.length} sensor ports, ` +
                `${description.inPorts.length} actuator ports`);
            scanner = new Scanner(system, env, clock, log, turn => link.turn(turn));
            link.serve({system, scanner, description});
            scanner.start();
        }
    }
    try {
        if (!stop.aborted)
            await new Promise<void>(resolve => stop.addEventListener('abort', () => resolve(), {once: true}));
    } finally {
        scanner?.stop();
        await link.close();
    }
    log.info('stopped');
}

// Reads and checks the environment to run (spec §5). One that cannot be run is logged, and the
// emulator runs without one until another is selected.
export async function openEnvironment(library: Library, name: string, catalog: Catalog,
                                      log: Logger): Promise<Environment | undefined> {
    const read = await library.read(name);
    const checked = read.env === undefined ? {errors: [], warnings: []} : checkEnvironment(read.env, catalog);
    for (const w of [...read.warnings, ...checked.warnings])
        log.warn(`environment ${name}: ${w}`);
    const errors = [...read.errors, ...checked.errors];
    for (const e of errors)
        log.error(`environment ${name}: ${e}`);
    if (read.env === undefined || errors.length > 0) {
        log.error(`environment ${name} cannot be run`);
        return undefined;
    }
    const env = read.env;
    log.info(`environment ${name}: carrier ${carrierOf(env, catalog)!.type}, ` +
        `modules ${env.modules.length}, cycle ${env.cycleMs} ms`);
    await library.setLastRun(name);
    return env;
}
