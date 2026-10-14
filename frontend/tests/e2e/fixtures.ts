// The end-to-end fixtures: every test gets its own hakkas engine — the real executable, on
// free loopback ports, with a fresh temporary configuration, users file and processes —
// serving the frontend's build (frontend/dist), and a browser page that fails the test on
// any page error.

import {test as base, expect, Page} from "@playwright/test";
import {ChildProcess, spawn} from "node:child_process";
import {randomBytes} from "node:crypto";
import {mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync} from "node:fs";
import {createServer} from "node:net";
import {tmpdir} from "node:os";
import {join} from "node:path";
import {makeUser, UserRecord} from "../support/scram-server";
import {ProtocolClient} from "./protocol-client";
import {DIST_DIR, EXAMPLE_GRAPH, HAKKAS_BIN} from "./paths";

export interface TestUser {
    user: string;
    password: string;
    role: UserRecord["role"];
}

// The users every engine has.
export const EDITOR: TestUser = {user: "alice", password: "alice-password", role: "editor"};
export const VIEWER: TestUser = {user: "victor", password: "victor-password", role: "viewer"};
export const OPERATOR: TestUser = {user: "olga", password: "olga-password", role: "operator"};

// A graph file's content (05 §2.1), or the path of one.
export type GraphSource = string | Record<string, unknown>;

export interface Engine {
    url: string;            // the page, http://127.0.0.1:<port>/
    port: number;
    dataDir: string;        // its datadir (graphs/, types/)
    log(): string;
    // A second client on the WebSocket, logged in (another user of the same engine).
    client(user?: TestUser): Promise<ProtocolClient>;
    // Stops the engine cleanly before the test ends (SIGINT).
    stop(): Promise<void>;
    // Stops it and starts it again on the same ports, with the same files.
    restart(): Promise<void>;
}

interface Options {
    graphs: GraphSource[];                  // the processes in <datadir>/graphs/
    config: Record<string, unknown>;        // merged into hakkas.json
    allowedErrors: RegExp[];                // error lines the test causes on purpose
}

async function portFree(port: number): Promise<boolean> {
    return new Promise(resolve => {
        const server = createServer();
        server.once("error", () => resolve(false));
        server.listen(port, "127.0.0.1", () => server.close(() => resolve(true)));
    });
}

// A port whose successor (the WebSocket, 02 §0) is free too.
async function freePortPair(): Promise<number> {
    for (let attempt = 0; attempt < 50; attempt++) {
        const port = 20000 + Math.floor(Math.random() * 40000);
        if (await portFree(port) && await portFree(port + 1))
            return port;
    }
    throw new Error("no free port pair");
}

function graphFile(source: GraphSource): Record<string, unknown> {
    return typeof source === "string" ? JSON.parse(readFileSync(source, "utf8")) : source;
}

async function startEngine(dir: string, port?: number, log: string[] = []): Promise<{ proc: ChildProcess, port: number, log: string[] }> {
    port ??= await freePortPair();
    const proc = spawn(HAKKAS_BIN, [
        "--info", "--confdir", join(dir, "conf"), "--basedir", join(dir, "data"),
        "run", "--host", "127.0.0.1", "--port", String(port), "--webdir", DIST_DIR, "daemon",
    ], {stdio: ["ignore", "pipe", "pipe"]});
    const collect = (chunk: Buffer) => log.push(...chunk.toString().split("\n").filter(l => l !== ""));
    proc.stdout!.on("data", collect);
    proc.stderr!.on("data", collect);
    // Ready once the host has loaded its processes (05 §1.2).
    await new Promise<void>((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error("the engine did not start:\n" + log.join("\n"))), 10000);
        const check = () => {
            if (log.some(l => / process(es)? loaded/.test(l))) {
                clearTimeout(timer);
                resolve();
            }
        };
        proc.stdout!.on("data", check);
        proc.stderr!.on("data", check);
        proc.once("exit", code => {
            clearTimeout(timer);
            reject(new Error(`the engine exited (${code}) at startup:\n` + log.join("\n")));
        });
    });
    return {proc, port, log};
}

async function stopEngine(proc: ChildProcess): Promise<number | null> {
    if (proc.exitCode !== null)
        return proc.exitCode;
    const exited = new Promise<number | null>(resolve => proc.once("exit", code => resolve(code)));
    proc.kill("SIGINT");
    const timer = setTimeout(() => proc.kill("SIGKILL"), 5000);
    const code = await exited;
    clearTimeout(timer);
    return code;
}

export const test = base.extend<Options & { engine: Engine, pageErrors: string[] }>({
    graphs: [[EXAMPLE_GRAPH], {option: true}],
    config: [{}, {option: true}],
    allowedErrors: [[], {option: true}],

    engine: async ({graphs, config, allowedErrors}, use, testInfo) => {
        const dir = mkdtempSync(join(tmpdir(), "hakkas-e2e-"));
        mkdirSync(join(dir, "conf"));
        mkdirSync(join(dir, "data", "graphs"), {recursive: true});
        const auth = {server_secret: randomBytes(32).toString("base64"), ...(config.auth as object ?? {})};
        writeFileSync(join(dir, "conf", "hakkas.json"), JSON.stringify({...config, auth}));
        writeFileSync(join(dir, "conf", "users.json"), JSON.stringify(
            [EDITOR, VIEWER, OPERATOR].map(u => makeUser(u.user, u.password, u.role))));
        for (const source of graphs) {
            const graph = graphFile(source);
            writeFileSync(join(dir, "data", "graphs", graph.id + ".json"), JSON.stringify(graph));
        }

        let {proc, port, log} = await startEngine(dir);
        const clients: ProtocolClient[] = [];
        await use({
            url: `http://127.0.0.1:${port}/`,
            port,
            dataDir: join(dir, "data"),
            log: () => log.join("\n"),
            client: async (user = EDITOR) => {
                const client = await ProtocolClient.connect(port, user.user, user.password);
                clients.push(client);
                return client;
            },
            stop: async () => {
                await stopEngine(proc);
            },
            restart: async () => {
                await stopEngine(proc);
                log.length = 0;
                ({proc} = await startEngine(dir, port, log));
            },
        });
        clients.forEach(c => c.close());
        const code = await stopEngine(proc);
        await testInfo.attach("engine.log", {body: log.join("\n"), contentType: "text/plain"});
        rmSync(dir, {recursive: true, force: true});
        // The engine stops cleanly (05 §1.2) and logged no error.
        expect(code, "the engine's exit code").toBe(0);
        expect(log.filter(l => l.startsWith("error") && !allowedErrors.some(e => e.test(l))),
            "errors in the engine's log").toEqual([]);
    },

    // Page errors and failed graph events, collected for every test and checked at its end.
    pageErrors: [async ({page}, use) => {
        const errors: string[] = [];
        page.on("pageerror", e => errors.push("page error: " + e.message));
        page.on("console", m => {
            if (m.text().startsWith("Graph event failed"))
                errors.push(m.text());
        });
        await use(errors);
        expect(errors, "errors in the page").toEqual([]);
    }, {auto: true}],
});

export {expect};
export type {Page};
