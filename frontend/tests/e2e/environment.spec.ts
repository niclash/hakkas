import type {Page} from "@playwright/test";
import {readFileSync} from "node:fs";
import {expect, test} from "./fixtures";
import {block, blocks, loggedIn, openProcess, processItem} from "./ui";

// 06 §4.14: the environment (02 §3.7) — its panel, binding process ports (plan task 70) from
// their markers' menus and from a block port's (exposing it first), the markers showing their
// bindings, undo, and a bindings file loaded (with the mapping dialog) or saved.

const G = "0826152e-3014-4c89-9380-ec4991a76af2";

const SIM = {
    name: "sim",
    description: "Test environment",
    outPorts: [{id: "slot1-ai", type: "number", addressable: true, count: 2, unit: "V"}, {id: "temp", type: "number"}],
    inPorts: [{id: "slot2-dq", type: "bool", addressable: true, count: 2}],
    modules: [{slot: 0, builtin: true, type: "c/IO", ports: []}, {slot: 1, type: "c_aiv", name: "C AIV", serial: "7", ports: ["slot1-ai"]},
        {slot: 2, type: "c/SSR", ports: ["slot2-dq"]}],
    carrier: {type: "c_8", name: "C 8", vendor: "Maker", slots: 7, builtin: [{slot: 0}],
        comm: [{slot: 1, type: "c_ethernet", name: "C Ethernet", ip: {mode: "dhcp", address: "10.0.0.5", prefix: 24}}],
        mcu: {type: "c_mcu6", name: "C MCU6", serial: "M6"}},
};

test.use({environment: SIM});

async function openExample(page: Page, url: string) {
    await loggedIn(page, url);
    await openProcess(page, "Simple Example");
    await expect(blocks(page)).toHaveCount(8);
}

const item = (page: Page, action: string, arg?: string) => page.getByTestId("context-menu")
    .locator(`[data-action="${action}"]` + (arg !== undefined ? `[data-arg="${arg}"]` : ""));
const row = (page: Page, id: string, side: "input" | "output", port: string) =>
    block(page, id).getByTestId(side + "-" + port).getByTestId(side + "-title");
const marker = (page: Page, direction: "in" | "out", name: string) =>
    page.locator(`[data-testid=port-node][data-direction="${direction}"][data-name="${name}"]`);
const channel = (page: Page, label: string) => page.locator(`[data-testid=env-channel][data-channel="${label}"]`);

// Binds a block port from its menu: through the process port exposing it, or a new one named
// `name` (06 §4.14).
async function bind(page: Page, id: string, side: "input" | "output", port: string, label: string, name?: string) {
    await row(page, id, side, port).click({button: "right"});
    await item(page, "bind").click();
    if (name !== undefined)
        await page.getByTestId("bind-port-name").fill(name);
    await page.getByTestId("bind-channel").selectOption(label);
    await page.getByTestId("dialog-ok").click();
    await expect(page.getByTestId("bind-channel")).toHaveCount(0);
}

test("the panel shows the environment; a port is bound and unbound from its menus, and undone", async ({page, engine}) => {
    await openExample(page, engine.url);
    await expect(page.getByTestId("env-name")).toHaveText("sim");
    // A name is shown, else the type; with a name the type is the tooltip (plan task 68).
    await expect(page.getByTestId("env-carrier")).toContainText("C 8 · Maker · 8 I/O slots");
    await expect(page.getByTestId("env-carrier")).toHaveAttribute("title", "c_8");
    await expect(page.getByTestId("env-comm")).toHaveText("Comm 1: C Ethernet · dhcp 10.0.0.5/24");
    await expect(page.getByTestId("env-mcu")).toHaveText("MCU: C MCU6 · serial M6");      // plan task 73
    await expect(page.getByTestId("env-mcu")).toHaveAttribute("title", "c_mcu6");
    await expect(page.getByTestId("env-module")).toHaveCount(4);   // 3 modules, the ports no module lists
    const summaries = page.getByTestId("env-module").locator("summary");
    await expect(summaries.nth(1)).toHaveText("Slot 1: C AIV");
    await expect(summaries.nth(1)).toHaveAttribute("title", "c_aiv · serial 7");
    await expect(summaries.nth(2)).toHaveText("Slot 2: c/SSR");
    const other = await engine.client();
    await other.connectGraph(G);

    // A block's input that no process port exposes: exposed as one (its name offered), which is
    // bound to a sensor.
    await row(page, "block1", "input", "in1").click({button: "right"});
    await item(page, "bind").click();
    await expect(page.getByTestId("bind-port-name")).toHaveValue("in1");
    await page.getByTestId("bind-port-name").fill("level");
    await page.getByTestId("bind-channel").selectOption("slot1-ai[2]");
    await page.getByTestId("dialog-ok").click();
    await expect(page.getByTestId("bind-channel")).toHaveCount(0);
    expect((await other.next(m => m.command === "addinport")).payload).toMatchObject({graph: G, name: "level", node: "block1", port: "in1"});
    expect((await other.next(m => m.command === "bind")).payload).toEqual({graph: G, port: "level", env: "slot1-ai", index: 2});
    await expect(marker(page, "in", "level")).toHaveAttribute("data-bound", "slot1-ai[2]");
    await expect(marker(page, "in", "level").getByTestId("port-binding")).toHaveText("⇄ slot1-ai[2]");
    await expect(page.locator("[data-testid=port-connection][data-bound=true]")).toHaveCount(1);
    await expect(channel(page, "slot1-ai[2]")).toHaveAttribute("data-bound", "level");
    await expect(processItem(page, "Simple Example").getByTestId("process-save")).toBeEnabled();   // unsaved

    // An output: an actuator; one driven already cannot be chosen. The engine writes it (04 §0.1).
    await bind(page, "block7", "output", "out", "slot2-dq[1]");
    await expect(marker(page, "out", "out")).toHaveAttribute("data-bound", "slot2-dq[1]");
    other.send("network", "start", {graph: G});
    await expect.poll(() => engine.environmentReceived()).toContain('"write":{"port":"slot2-dq","index":1');
    await row(page, "block6", "output", "out").click({button: "right"});
    await item(page, "bind").click();
    await expect(page.getByTestId("bind-port-name")).toHaveValue("block6_out");   // "out" is taken
    await expect(page.getByTestId("bind-channel").locator("option[value='slot2-dq[1]']")).toBeDisabled();
    await expect(page.getByTestId("bind-channel")).toHaveValue("slot2-dq[2]");
    await page.getByTestId("dialog-cancel").click();
    // An outport may drive several actuators: bound again from its marker's menu.
    await marker(page, "out", "out").click({button: "right"});
    await item(page, "bind").click();
    await expect(page.getByTestId("bind-port-name")).toHaveCount(0);
    await page.getByTestId("dialog-ok").click();
    await expect(marker(page, "out", "out")).toHaveAttribute("data-bound", "slot2-dq[1], slot2-dq[2]");
    await expect(marker(page, "out", "out").getByTestId("port-binding")).toHaveText("⇄ slot2-dq[1] +1");

    // The inspector says where a bound input's value comes from.
    await block(page, "block1").getByTestId("title").click();
    await expect(page.getByTestId("input-value-in1").getByTestId("input-state")).toHaveText("From the environment: slot1-ai[2]");

    // Unbound from the marker's menu: the port stays. Undone and redone.
    await marker(page, "in", "level").click({button: "right"});
    await expect(item(page, "bind")).toHaveCount(0);   // a bound inport has its source
    await item(page, "unbind", "slot1-ai[2]").click();
    expect((await other.next(m => m.command === "unbind")).payload).toEqual({graph: G, port: "level", env: "slot1-ai", index: 2});
    await expect(marker(page, "in", "level")).toHaveAttribute("data-bound", "");
    await expect(channel(page, "slot1-ai[2]")).toHaveAttribute("data-bound", "");
    await page.locator("#hk-editor").click({position: {x: 5, y: 5}});
    await page.keyboard.press("Control+z");
    await expect(marker(page, "in", "level")).toHaveAttribute("data-bound", "slot1-ai[2]");
    await page.keyboard.press("Control+Shift+z");
    await expect(marker(page, "in", "level")).toHaveAttribute("data-bound", "");

    // Exposing and binding was one step: undone, the port goes with its binding.
    await page.keyboard.press("Control+z");   // the unbinding
    await page.keyboard.press("Control+z");   // the second actuator
    await page.keyboard.press("Control+z");   // block7.out exposed and bound
    await expect(marker(page, "out", "out")).toHaveCount(0);
    await expect(channel(page, "slot2-dq[1]")).toHaveAttribute("data-bound", "");
    await expect(marker(page, "in", "level")).toHaveAttribute("data-bound", "slot1-ai[2]");
});

test("a refused binding takes back the process port made for it", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    await other.connectGraph(G);
    await row(page, "block6", "output", "out").click({button: "right"});
    await item(page, "bind").click();
    await page.getByTestId("bind-channel").selectOption("slot2-dq[2]");
    // Meanwhile another user drives that channel.
    other.send("graph", "addoutport", {graph: G, node: "block5", port: "out", public: "spare"});
    await other.next(m => m.command === "addoutport");
    other.send("environment", "bind", {graph: G, port: "spare", env: "slot2-dq", index: 2});
    await other.next(m => m.command === "bind");
    await page.getByTestId("dialog-ok").click();
    await expect(page.getByTestId("bind-error")).toHaveText("Actuator already driven: slot2-dq[2]");
    await expect(marker(page, "out", "out")).toHaveCount(0);   // made, and taken back
    await page.getByTestId("dialog-cancel").click();
    // Nothing to undo of it.
    await page.locator("#hk-editor").click({position: {x: 5, y: 5}});
    await page.keyboard.press("Control+z");
    await expect(marker(page, "out", "spare")).toHaveAttribute("data-bound", "slot2-dq[2]");
});

test("bindings are saved with the process, follow a renamed port, and come back after a restart", async ({page, engine}) => {
    await openExample(page, engine.url);
    await bind(page, "block2", "input", "in3", "temp", "temperature");
    await processItem(page, "Simple Example").getByTestId("process-save").click();
    await expect(processItem(page, "Simple Example").getByTestId("process-save")).toBeDisabled();
    const file = () => JSON.parse(readFileSync(engine.dataDir + "/bindings/" + G + ".json", "utf8"));
    expect(file()).toEqual({environment: "sim", bindings: [{port: "temperature", env: "temp"}]});
    // Renamed, the port keeps its binding (04 §4).
    await marker(page, "in", "temperature").click({button: "right"});
    await item(page, "rename-port").click();
    await page.getByTestId("port-name-field").fill("t");
    await page.getByTestId("dialog-ok").click();
    await expect(marker(page, "in", "t")).toHaveAttribute("data-bound", "temp");
    await expect(channel(page, "temp")).toHaveAttribute("data-bound", "t");
    await processItem(page, "Simple Example").getByTestId("process-save").click();
    await expect(processItem(page, "Simple Example").getByTestId("process-save")).toBeDisabled();
    expect(file()).toEqual({environment: "sim", bindings: [{port: "t", env: "temp"}]});
    await engine.restart();
    await loggedIn(page, engine.url);   // a restart ends every session
    await openProcess(page, "Simple Example");
    await expect(marker(page, "in", "t")).toHaveAttribute("data-bound", "temp");
    await expect(processItem(page, "Simple Example").getByTestId("process-save")).toBeDisabled();
});

test("a bindings file that fits is applied; one that does not opens the mapping dialog", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    for (const [command, node, port, name] of [["addinport", "block1", "in1", "level"], ["addoutport", "block7", "out", "valve"],
        ["addoutport", "block8", "out", "lamp"]]) {
        other.send("graph", command, {graph: G, node, port, public: name});
        await other.next(m => m.command === command && m.payload.name === name);
    }
    await bind(page, "block4", "input", "in3", "temp", "spare");   // replaced by the file's
    const file = (name: string, content: object) =>
        ({name, mimeType: "application/json", buffer: Buffer.from(JSON.stringify(content))});

    await page.getByTestId("env-file").setInputFiles(file("ok.json", {environment: "sim", bindings: [
        {port: "level", env: "slot1-ai", index: 1}, {port: "valve", env: "slot2-dq", index: 2}]}));
    await expect(page.getByTestId("env-message")).toHaveText("ok.json: 2 bindings applied");
    await expect(marker(page, "in", "level")).toHaveAttribute("data-bound", "slot1-ai[1]");
    await expect(marker(page, "out", "valve")).toHaveAttribute("data-bound", "slot2-dq[2]");
    await expect(marker(page, "in", "spare")).toHaveAttribute("data-bound", "");
    await expect(page.getByTestId("mapping-dialog")).toHaveCount(0);

    // Written for the emulator, with a channel and a process port this process lacks.
    await page.getByTestId("env-file").setInputFiles(file("emu.json", {environment: "emulator", bindings: [
        {port: "level", env: "slot1-ai", index: 3},
        {port: "gone", env: "temp"},
        {port: "lamp", env: "slot2-dq", index: 1}]}));
    const dialog = page.getByTestId("mapping-dialog");
    await expect(dialog.getByTestId("mapping-mismatch")).toContainText("“emulator”");
    const rows = dialog.getByTestId("mapping-row");
    await expect(rows.nth(0).getByTestId("mapping-problem")).toHaveText("Choose a channel");
    await expect(rows.nth(1).getByTestId("mapping-problem")).toHaveText("Choose a process port");
    await expect(rows.nth(2).getByTestId("mapping-problem")).toHaveText("");
    await expect(dialog.getByTestId("mapping-apply")).toBeDisabled();
    await rows.nth(0).getByTestId("mapping-channel").selectOption("slot1-ai[2]");
    await rows.nth(1).getByTestId("mapping-port").selectOption("level");
    await expect(rows.nth(1).getByTestId("mapping-problem")).toHaveText("Input already connected: level");
    await rows.nth(1).getByTestId("mapping-skip").check();
    await dialog.getByTestId("mapping-apply").click();
    await expect(dialog).toHaveCount(0);
    await expect(page.getByTestId("env-message")).toHaveText("emu.json: 2 bindings applied");
    await expect(marker(page, "in", "level")).toHaveAttribute("data-bound", "slot1-ai[2]");
    await expect(marker(page, "out", "lamp")).toHaveAttribute("data-bound", "slot2-dq[1]");
    await expect(marker(page, "out", "valve")).toHaveAttribute("data-bound", "");

    // Applying a file is one step.
    await page.locator("#hk-editor").click({position: {x: 5, y: 5}});
    await page.keyboard.press("Control+z");
    await expect(marker(page, "out", "valve")).toHaveAttribute("data-bound", "slot2-dq[2]");
    await expect(marker(page, "out", "lamp")).toHaveAttribute("data-bound", "");
    await expect(marker(page, "in", "level")).toHaveAttribute("data-bound", "slot1-ai[1]");

    // Not a bindings file; the earlier form, which bound block ports (plan task 70).
    await page.getByTestId("env-file").setInputFiles(file("bad.json", {bindings: []}));
    await expect(page.getByTestId("env-message")).toHaveText("bad.json: environment: expected a non-empty string");
    await page.getByTestId("env-file").setInputFiles(file("old.json", {environment: "sim", bindings: [
        {block: "block1", port: "in1", env: "temp"}]}));
    await expect(page.getByTestId("env-message"))
        .toHaveText("old.json: bindings[0]: block: block ports are not bound; bind a process port");
});

test("the process's bindings are saved as a bindings file", async ({page, engine}) => {
    await openExample(page, engine.url);
    await bind(page, "block1", "input", "in2", "slot1-ai[1]");
    const download = page.waitForEvent("download");
    await page.getByTestId("env-save").click();
    const d = await download;
    expect(d.suggestedFilename()).toBe("Simple_Example.bindings.json");
    expect(JSON.parse(readFileSync(await d.path(), "utf8")))
        .toEqual({environment: "sim", bindings: [{port: "in2", env: "slot1-ai", index: 1}]});
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
