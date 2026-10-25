import type {Page} from "@playwright/test";
import {readFileSync} from "node:fs";
import {expect, test} from "./fixtures";
import {block, blocks, loggedIn, openProcess, processItem} from "./ui";

// 06 §4.14: the environment (02 §3.7) — its panel, binding block ports from their menus, the
// markers, undo, and a bindings file loaded (with the mapping dialog) or saved.

const G = "0826152e-3014-4c89-9380-ec4991a76af2";

const SIM = {
    name: "sim",
    description: "Test environment",
    outPorts: [{id: "slot1-ai", type: "number", addressable: true, count: 2, unit: "V"}, {id: "temp", type: "number"}],
    inPorts: [{id: "slot2-dq", type: "bool", addressable: true, count: 2}],
    modules: [{slot: 0, builtin: true, type: "c/IO", ports: []}, {slot: 1, type: "c/AIV", serial: "7", ports: ["slot1-ai"]},
        {slot: 2, type: "c/SSR", ports: ["slot2-dq"]}],
    carrier: {type: "c/C8", vendor: "Maker", slots: 7, builtin: [{slot: 0}],
        comm: [{slot: 1, type: "c/Ethernet", ip: {mode: "dhcp", address: "10.0.0.5", prefix: 24}}]},
};

test.use({environment: SIM});

async function openExample(page: Page, url: string) {
    await loggedIn(page, url);
    await openProcess(page, "Simple Example");
    await expect(blocks(page)).toHaveCount(8);
}

const item = (page: Page, action: string) => page.getByTestId("context-menu").locator(`[data-action="${action}"]`);
const row = (page: Page, id: string, side: "input" | "output", port: string) =>
    block(page, id).getByTestId(side + "-" + port).getByTestId(side + "-title");
const marker = (page: Page, target: string) => page.locator(`[data-testid=binding-node][data-target="${target}"]`);
const channel = (page: Page, label: string) => page.locator(`[data-testid=env-channel][data-channel="${label}"]`);

async function bind(page: Page, id: string, side: "input" | "output", port: string, label: string) {
    await row(page, id, side, port).click({button: "right"});
    await item(page, "bind").click();
    await page.getByTestId("bind-channel").selectOption(label);
    await page.getByTestId("dialog-ok").click();
    await expect(page.getByTestId("bind-channel")).toHaveCount(0);
}

test("the panel shows the environment; a port is bound and unbound from its menu, and undone", async ({page, engine}) => {
    await openExample(page, engine.url);
    await expect(page.getByTestId("env-name")).toHaveText("sim");
    await expect(page.getByTestId("env-carrier")).toContainText("c/C8 · Maker · 8 I/O slots");
    await expect(page.getByTestId("env-comm")).toHaveText("Comm 1: c/Ethernet · dhcp 10.0.0.5/24");
    await expect(page.getByTestId("env-module")).toHaveCount(4);   // 3 modules, the ports no module lists
    const other = await engine.client();
    await other.connectGraph(G);

    // An input: a sensor.
    await bind(page, "block1", "input", "in1", "slot1-ai[2]");
    expect((await other.next(m => m.command === "bind")).payload)
        .toEqual({graph: G, block: "block1", port: "in1", env: "slot1-ai", index: 2});
    await expect(marker(page, "block1.in1")).toHaveAttribute("data-name", "slot1-ai[2]");
    await expect(page.getByTestId("binding-connection")).toHaveCount(1);
    await expect(channel(page, "slot1-ai[2]")).toHaveAttribute("data-bound", "block1.in1");
    await expect(processItem(page, "Simple Example").getByTestId("process-save")).toBeEnabled();   // unsaved

    // An output: an actuator; one driven already cannot be chosen.
    await bind(page, "block7", "output", "out", "slot2-dq[1]");
    await expect(marker(page, "block7.out")).toHaveAttribute("data-name", "slot2-dq[1]");
    await row(page, "block6", "output", "out").click({button: "right"});
    await item(page, "bind").click();
    await expect(page.getByTestId("bind-channel").locator("option[value='slot2-dq[1]']")).toBeDisabled();
    await expect(page.getByTestId("bind-channel")).toHaveValue("slot2-dq[2]");
    await page.getByTestId("dialog-cancel").click();

    // The inspector says where a bound input's value comes from.
    await block(page, "block1").getByTestId("title").click();
    await expect(page.getByTestId("input-value-in1").getByTestId("input-state")).toHaveText("From the environment: slot1-ai[2]");

    // Unbound from the marker's menu, then undone and redone.
    await marker(page, "block1.in1").click({button: "right"});
    await item(page, "unbind").click();
    expect((await other.next(m => m.command === "unbind")).payload).toMatchObject({block: "block1", port: "in1"});
    await expect(marker(page, "block1.in1")).toHaveCount(0);
    await expect(channel(page, "slot1-ai[2]")).toHaveAttribute("data-bound", "");
    await page.locator("#hk-editor").click({position: {x: 5, y: 5}});
    await page.keyboard.press("Control+z");
    await expect(marker(page, "block1.in1")).toHaveAttribute("data-name", "slot1-ai[2]");
    await page.keyboard.press("Control+Shift+z");
    await expect(marker(page, "block1.in1")).toHaveCount(0);

    // The engine writes the actuator's channel (04 §0.1).
    other.send("network", "start", {graph: G});
    await expect.poll(() => engine.environmentReceived()).toContain('"write":{"port":"slot2-dq","index":1');
});

test("bindings are saved with the process and come back after a restart", async ({page, engine}) => {
    await openExample(page, engine.url);
    await bind(page, "block2", "input", "in3", "temp");
    await processItem(page, "Simple Example").getByTestId("process-save").click();
    await expect(processItem(page, "Simple Example").getByTestId("process-save")).toBeDisabled();
    const saved = JSON.parse(readFileSync(engine.dataDir + "/bindings/" + G + ".json", "utf8"));
    expect(saved).toEqual({environment: "sim", bindings: [{block: "block2", port: "in3", env: "temp"}]});
    await engine.restart();
    await loggedIn(page, engine.url);   // a restart ends every session
    await openProcess(page, "Simple Example");
    await expect(marker(page, "block2.in3")).toHaveAttribute("data-name", "temp");
    await expect(processItem(page, "Simple Example").getByTestId("process-save")).toBeDisabled();
});

test("a bindings file that fits is applied; one that does not opens the mapping dialog", async ({page, engine}) => {
    await openExample(page, engine.url);
    await bind(page, "block4", "input", "in3", "temp");   // replaced by the file's
    const file = (name: string, content: object) =>
        ({name, mimeType: "application/json", buffer: Buffer.from(JSON.stringify(content))});

    await page.getByTestId("env-file").setInputFiles(file("ok.json", {environment: "sim", bindings: [
        {block: "block1", port: "in1", env: "slot1-ai", index: 1}, {block: "block7", port: "out", env: "slot2-dq", index: 2}]}));
    await expect(page.getByTestId("env-message")).toHaveText("ok.json: 2 bindings applied");
    await expect(marker(page, "block1.in1")).toHaveCount(1);
    await expect(marker(page, "block7.out")).toHaveCount(1);
    await expect(marker(page, "block4.in3")).toHaveCount(0);
    await expect(page.getByTestId("mapping-dialog")).toHaveCount(0);

    // Written for the emulator, with a channel and a block port this process lacks.
    await page.getByTestId("env-file").setInputFiles(file("emu.json", {environment: "emulator", bindings: [
        {block: "block1", port: "in1", env: "slot1-ai", index: 3},
        {block: "gone", port: "in1", env: "temp"},
        {block: "block8", port: "out", env: "slot2-dq", index: 1}]}));
    const dialog = page.getByTestId("mapping-dialog");
    await expect(dialog.getByTestId("mapping-mismatch")).toContainText("“emulator”");
    const rows = dialog.getByTestId("mapping-row");
    await expect(rows.nth(0).getByTestId("mapping-problem")).toHaveText("Choose a channel");
    await expect(rows.nth(1).getByTestId("mapping-problem")).toHaveText("Choose a block port");
    await expect(rows.nth(2).getByTestId("mapping-problem")).toHaveText("");
    await expect(dialog.getByTestId("mapping-apply")).toBeDisabled();
    await rows.nth(0).getByTestId("mapping-channel").selectOption("slot1-ai[2]");
    await rows.nth(1).getByTestId("mapping-port").selectOption("block1.in1");
    await expect(rows.nth(1).getByTestId("mapping-problem")).toHaveText("Input already connected: block1.in1");
    await rows.nth(1).getByTestId("mapping-skip").check();
    await dialog.getByTestId("mapping-apply").click();
    await expect(dialog).toHaveCount(0);
    await expect(page.getByTestId("env-message")).toHaveText("emu.json: 2 bindings applied");
    await expect(marker(page, "block1.in1")).toHaveAttribute("data-name", "slot1-ai[2]");
    await expect(marker(page, "block8.out")).toHaveAttribute("data-name", "slot2-dq[1]");
    await expect(marker(page, "block7.out")).toHaveCount(0);

    // Applying a file is one step.
    await page.locator("#hk-editor").click({position: {x: 5, y: 5}});
    await page.keyboard.press("Control+z");
    await expect(marker(page, "block7.out")).toHaveAttribute("data-name", "slot2-dq[2]");
    await expect(marker(page, "block8.out")).toHaveCount(0);
    await expect(marker(page, "block1.in1")).toHaveAttribute("data-name", "slot1-ai[1]");

    // Not a bindings file.
    await page.getByTestId("env-file").setInputFiles(file("bad.json", {bindings: []}));
    await expect(page.getByTestId("env-message")).toHaveText("bad.json: environment: expected a non-empty string");
});

test("the process's bindings are saved as a bindings file", async ({page, engine}) => {
    await openExample(page, engine.url);
    await bind(page, "block1", "input", "in2", "slot1-ai[1]");
    const download = page.waitForEvent("download");
    await page.getByTestId("env-save").click();
    const d = await download;
    expect(d.suggestedFilename()).toBe("Simple_Example.bindings.json");
    expect(JSON.parse(readFileSync(await d.path(), "utf8")))
        .toEqual({environment: "sim", bindings: [{block: "block1", port: "in2", env: "slot1-ai", index: 1}]});
});

test.describe("without an environment", () => {
    test.use({environment: undefined});

    test("no panel and no binding in the menus", async ({page, engine}) => {
        await openExample(page, engine.url);
        await expect(page.getByTestId("env-panel")).toHaveCount(0);
        await row(page, "block1", "input", "in1").click({button: "right"});
        await expect(item(page, "set-value")).toBeVisible();
        await expect(item(page, "bind")).toHaveCount(0);
        await expect(page.getByTestId("error")).toHaveCount(0);
    });
});
