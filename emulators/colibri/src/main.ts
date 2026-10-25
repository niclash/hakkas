#!/usr/bin/env node
// The colibri-emulator command (spec §3.1).
import os from 'node:os';
import {parseCommandLine, USAGE} from './cli.js';
import {runEmulator} from './emulator.js';
import {createLogger} from './log.js';
import {VERSION} from './package.js';

async function main(): Promise<number> {
    const command = parseCommandLine(process.argv.slice(2), {
        env: process.env, uid: process.getuid?.() ?? 0, tmpdir: os.tmpdir(),
    });
    switch (command.kind) {
    case 'help':
        process.stdout.write(USAGE);
        return 0;
    case 'version':
        process.stdout.write(`colibri-emulator ${VERSION}\n`);
        return 0;
    case 'error':
        process.stderr.write(`colibri-emulator: ${command.message}\n${USAGE}`);
        return 2;
    }

    const log = createLogger(command.options.logLevel);
    const stop = new AbortController();
    for (const signal of ['SIGINT', 'SIGTERM'] as const)
        process.on(signal, () => stop.abort());
    try {
        await runEmulator(command.options, log, stop.signal);
        return 0;
    } catch (e) {
        log.error((e as Error).message);
        log.error('the emulator did not start');
        return 1;
    }
}

process.exitCode = await main();
