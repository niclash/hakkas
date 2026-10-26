import type {Page} from "@playwright/test";
import {existsSync, readFileSync} from "node:fs";
import {join} from "node:path";
import {expect, test} from "./fixtures";
import {blocks, loggedIn, openProcess, processItem} from "./ui";

// 06 §4.16: a process downloaded with its bindings and the composite types it uses, uploaded
// again (replacing it, its bindings applied or mapped), a backup restored; over a second
// connection, so the editor is left as it was.

const G = "0826152e-3014-4c89-9380-ec4991a76af2";

const SIM = {
    name: "sim",
    outPorts: [{id: "slot1-ai", type: "number", addressable: true, count: 2, unit: "V"}],
    inPorts: [{id: "slot2-dq", type: "bool", addressable: true, count: 2}],
    modules: [],
};

async function downloaded(page: Page, click: () => Promise<void>): Promise<{ name: string, file: any }> {
    const download = page.waitForEvent("download");
    await click();
    const d = await download;
    return {name: d.suggestedFilename(), file: JSON.parse(readFileSync(await d.path(), "utf8"))};
}

async function upload(page: Page, file: object, name = "x.process.json") {
    await page.getByTestId("process-upload-file").setInputFiles({name, mimeType: "application/json",
        buffer: Buffer.from(JSON.stringify(file))});
    await expect(page.getByTestId("transfer-upload")).toBeEnabled();
}

const report = (page: Page, id: string) => page.locator(`[data-testid=transfer-report][data-id="${id}"]`);

test.describe("with an environment", () => {
    test.use({environment: SIM});

    test("download a process with its bindings and type; upload it again, replacing it, and with a mapping",
        async ({page, engine}) => {
        const other = await engine.client();
        await other.command("graph", "clear", {id: "user/Twice", name: "Twice", main: false});
        other.send("graph", "addnode", {graph: "user/Twice", id: "m", component: "math/Add2", metadata: {x: 0, y: 0}});
        other.send("graph", "addinport", {graph: "user/Twice", public: "in", node: "m", port: "in1"});
        other.send("graph", "addoutport", {graph: "user/Twice", public: "out", node: "m", port: "out"});
        other.send("graph", "addinitial", {graph: "user/Twice", src: {data: "2"}, tgt: {node: "m", port: "in2"}});
        other.send("network", "persist", {graph: "user/Twice"});
        await other.next(m => m.command === "component" && m.payload.name === "user/Twice");
        other.send("graph", "addnode", {graph: G, id: "tw", component: "user/Twice", metadata: {x: 900, y: 50}});
        other.send("graph", "addinport", {graph: G, public: "level", node: "block1", port: "in1"});
        expect((await other.command("environment", "bind", {graph: G, port: "level", env: "slot1-ai", index: 2})).command)
            .toBe("bind");

        await loggedIn(page, engine.url);
        await openProcess(page, "Simple Example");
        await expect(blocks(page)).toHaveCount(9);

        const {name, file} = await downloaded(page, () => processItem(page, "Simple Example").getByTestId("process-download").click());
        expect(name).toBe("Simple Example.process.json");
        expect(file).toMatchObject({format: "hakkas/processes", version: 1, runtime: {label: "Hakkas"}});
        expect(file.processes).toHaveLength(1);
        const p = file.processes[0];
        expect(p.graph.id).toBe(G);
        expect(Object.keys(p.graph.blocks)).toHaveLength(9);
        expect(p.graph.blocks.tw).toEqual({component: "user/Twice", metadata: {x: 900, y: 50}});
        expect(p.graph.inports).toEqual([{name: "level", block: "block1", port: "in1"}]);
        expect(p.bindings).toEqual({environment: "sim", bindings: [{port: "level", env: "slot1-ai", index: 2}]});
        expect(file.types).toHaveLength(1);
        expect(file.types[0]).toMatchObject({id: "user/Twice", library: "user", inports: [{name: "in", block: "m", port: "in1"}],
            connections: [{src: {data: "2"}, tgt: {block: "m", port: "in2"}}]});
        // The editor was left alone.
        await expect(processItem(page, "Simple Example")).toHaveClass(/active/);
        await expect(blocks(page)).toHaveCount(9);

        // Uploaded again: it replaces the process; the type is already there.
        await upload(page, file);
        await expect(page.getByTestId("transfer-action")).toContainText("replaces “Simple Example”");
        await expect(page.getByTestId("transfer-type-action")).toHaveText("already on this engine");
        await expect(page.getByTestId("transfer-bindings")).toHaveText("Bindings for “sim”");
        await expect(page.getByTestId("transfer-upload")).toHaveText("Replace and upload");
        await page.getByTestId("transfer-upload").click();
        await expect(report(page, G)).toContainText("uploaded, stopped");
        await expect(report(page, G).getByTestId("transfer-note")).toHaveText(["1 binding applied"]);
        await page.getByTestId("transfer-close").click();
        await expect(page.getByTestId("transfer-dialog")).toHaveCount(0);
        await expect(processItem(page, "Simple Example")).toHaveClass(/active/);
        await expect(blocks(page)).toHaveCount(9);
        await expect(page.locator('[data-testid=port-node][data-name="level"]')).toHaveAttribute("data-bound", "slot1-ai[2]");

        // On the engine: the same process, stopped and saved, with its binding.
        const snapshot = await other.connectGraph(G);
        expect(snapshot.filter(m => m.command === "addnode")).toHaveLength(9);
        other.send("network", "getstatus", {graph: G});
        const status = await other.next(m => m.command === "status" && m.payload.graph === G);
        expect(status.payload).toMatchObject({running: false, unsaved: false});
        expect(JSON.parse(readFileSync(join(engine.dataDir, "graphs", G + ".json"), "utf8")).started).toBe(false);
        expect(existsSync(join(engine.dataDir, "bindings", G + ".json"))).toBe(true);
        other.send("environment", "getbindings", {});
        const bound = await other.next(m => m.command === "bindings");
        expect(bound.payload.bindings).toEqual([{graph: G, port: "level", env: "slot1-ai", index: 2}]);

        // Written for another environment: the mapping dialog, then the binding applied.
        await upload(page, {...file, processes: [{...p, bindings: {environment: "lab", bindings: [{port: "level", env: "ai", index: 1}]}}]});
        await page.getByTestId("transfer-upload").click();
        await expect(page.getByTestId("mapping-mismatch")).toContainText("“lab”");
        await page.getByTestId("mapping-channel").selectOption("slot1-ai[1]");
        await page.getByTestId("mapping-apply").click();
        await expect(report(page, G).getByTestId("transfer-note")).toHaveText(["1 binding applied"]);
        await page.getByTestId("transfer-close").click();
        await expect(page.locator('[data-testid=port-node][data-name="level"]')).toHaveAttribute("data-bound", "slot1-ai[1]");
    });
});

test("back up, then restore a deleted process; a block type the engine lacks is refused, the rest made",
    async ({page, engine}) => {
    await loggedIn(page, engine.url);
    const {name, file} = await downloaded(page, () => page.getByTestId("process-backup").click());
    expect(name).toMatch(/^Hakkas \d{4}-\d\d-\d\d\.backup\.json$/);
    expect(file.processes.map((p: any) => p.graph.id)).toEqual([G]);
    expect(file.processes[0].bindings).toBeUndefined();
    expect(file.types).toEqual([]);

    const other = await engine.client();
    expect((await other.command("runtime", "delete_graph", {id: G, name: "Simple Example"})).command).toBe("delete_graph");
    await expect(processItem(page, "Simple Example")).toHaveCount(0);

    const g = file.processes[0].graph;
    g.blocks.zap = {component: "x/Zap", metadata: {x: 0, y: 0}};
    await upload(page, file, name);
    await expect(page.getByTestId("transfer-action")).toHaveText("new");
    await expect(page.getByTestId("transfer-missing")).toContainText("x/Zap");
    await page.getByTestId("transfer-upload").click();
    await expect(report(page, G)).toContainText("uploaded, stopped");
    await expect(report(page, G).getByTestId("transfer-note")).toContainText(["x/Zap"]);
    await page.getByTestId("transfer-close").click();
    await expect(processItem(page, "Simple Example")).toHaveClass(/active/);
    await expect(blocks(page)).toHaveCount(8);
    await expect(processItem(page, "Simple Example").getByTestId("process-unsaved")).toHaveCount(0);
});

test("a file that is none is said; a viewer may download but not upload", async ({page, engine}) => {
    await loggedIn(page, engine.url);
    await page.getByTestId("process-upload-file").setInputFiles({name: "x.json", mimeType: "application/json",
        buffer: Buffer.from("[1]")});
    await expect(page.getByTestId("transfer-message")).toHaveText("x.json: Not a process file: expected an object");
    await page.getByTestId("logout").click();
    await loggedIn(page, engine.url, {user: "victor", password: "victor-password", role: "viewer"});
    await expect(page.getByTestId("process-upload")).toHaveCount(0);
    const {file} = await downloaded(page, () => page.getByTestId("process-backup").click());
    expect(file.processes).toHaveLength(1);
});
