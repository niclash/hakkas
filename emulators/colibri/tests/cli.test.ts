import {describe, expect, it} from 'vitest';
import {DEFAULT_HTTP_PORT, parseCommandLine, type Command, type Host, type Options} from '../src/cli.js';

const host: Host = {env: {HOME: '/home/u', XDG_RUNTIME_DIR: '/run/user/1000'}, uid: 1000, tmpdir: '/tmp'};

function run(argv: string[], h: Host = host): Options {
    const c = parseCommandLine(argv, h);
    if (c.kind !== 'run')
        throw new Error(`expected run, got ${JSON.stringify(c)}`);
    return c.options;
}

function error(argv: string[], h: Host = host): string {
    const c: Command = parseCommandLine(argv, h);
    if (c.kind !== 'error')
        throw new Error(`expected an error, got ${JSON.stringify(c)}`);
    return c.message;
}

describe('the command line', () => {
    it('has defaults from the XDG directories', () => {
        expect(run([])).toEqual({
            library: '/home/u/.local/share/colibri-emulator',
            plugins: [],
            socket: '/run/user/1000/colibri-emulator/environment.sock',
            controlSocket: '/run/user/1000/colibri-emulator/control.sock',
            httpPort: DEFAULT_HTTP_PORT,
            mcu: 'colibri_mcu6',
            pictures: undefined,
            seed: undefined,
            logLevel: 'info',
            environment: undefined,
        });
    });

    it('prefers XDG_DATA_HOME to HOME', () => {
        expect(run([], {...host, env: {...host.env, XDG_DATA_HOME: '/data'}}).library)
            .toBe('/data/colibri-emulator');
    });

    it('puts the sockets in a per-user temporary directory without XDG_RUNTIME_DIR', () => {
        const o = run([], {...host, env: {HOME: '/home/u'}});
        expect(o.socket).toBe('/tmp/colibri-emulator-1000/environment.sock');
        expect(o.controlSocket).toBe('/tmp/colibri-emulator-1000/control.sock');
    });

    it('needs a library directory', () => {
        expect(error([], {...host, env: {}})).toMatch(/no library directory/);
        expect(run(['--library', '/lib'], {...host, env: {}}).library).toBe('/lib');
    });

    it('takes every option', () => {
        expect(run(['--library', 'lib', '--plugins', 'a', '--plugins', 'b', '--socket', 'e.sock',
            '--control-socket', 'c.sock', '--http', '0', '--mcu', 'colibri_mcu7', '--pictures', 'pics',
            '--seed', '4294967295', '--log-level', 'fine', 'lab-1'])).toEqual({
            library: 'lib', plugins: ['a', 'b'], socket: 'e.sock', controlSocket: 'c.sock', httpPort: 0,
            mcu: 'colibri_mcu7', pictures: 'pics', seed: 4294967295, logLevel: 'fine', environment: 'lab-1',
        });
    });

    it('answers help and version', () => {
        expect(parseCommandLine(['-h'], host)).toEqual({kind: 'help'});
        expect(parseCommandLine(['--help', 'x'], host)).toEqual({kind: 'help'});
        expect(parseCommandLine(['--version'], host)).toEqual({kind: 'version'});
    });

    it('refuses unknown options and missing values', () => {
        expect(error(['--frobnicate'])).toMatch(/frobnicate/);
        expect(error(['--http'])).toMatch(/http/);
    });

    it('refuses invalid values', () => {
        for (const p of ['65536', '-1', '80x', '', '1e3'])
            expect(error([`--http=${p}`])).toMatch(/invalid --http/);
        for (const s of ['4294967296', '-1', 'abc'])
            expect(error([`--seed=${s}`])).toMatch(/invalid --seed/);
        expect(error(['--log-level', 'debug'])).toMatch(/invalid --log-level/);
        for (const m of ['', 'MCU6', 'colibri-mcu6'])
            expect(error([`--mcu=${m}`])).toMatch(/invalid --mcu/);
        expect(error(['--plugins', ''])).toMatch(/empty directory/);
        expect(error(['--socket', ''])).toMatch(/empty socket/);
    });

    it('checks the environment name', () => {
        for (const name of ['.hidden', 'a/b', 'a b', 'x'.repeat(65)])
            expect(error([name])).toMatch(/invalid environment name/);
        expect(run(['x'.repeat(64)]).environment).toBe('x'.repeat(64));
        expect(error(['a', 'b'])).toMatch(/at most one environment/);
    });

    it('refuses socket paths a Unix socket address cannot hold', () => {
        expect(run(['--socket', '/' + 'x'.repeat(106)]).socket).toHaveLength(107);
        expect(error(['--socket', '/' + 'x'.repeat(107)])).toMatch(/longer than 107 bytes/);
        expect(error(['--control-socket', '/' + 'ä'.repeat(54)])).toMatch(/longer than 107 bytes/);
    });

    it('refuses the same path for both sockets', () => {
        expect(error(['--socket', '/s/a.sock', '--control-socket', '/s/../s/a.sock'])).toMatch(/are the same/);
    });
});
