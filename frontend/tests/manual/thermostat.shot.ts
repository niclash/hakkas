// Chapter 4, Building the thermostat (examples/thermostat): the process built step by step in
// the editor as the chapter describes it, on the example's Colibri 8 (a Pt1000 and a relay), with
// a screenshot at each step. The schedule's blocks (4.8) are made by a second client, as the
// chapter lists them; their last connection is drawn in the editor.
import {readFileSync} from "node:fs";
import {expect, test} from "../e2e/fixtures";
import {block, loggedIn, processItem} from "../e2e/ui";
import {devicePictures, lightTheme, mark, menu, offline, shot, shotOfProcess} from "./shots";

const example = JSON.parse(readFileSync(new URL("../../../examples/thermostat/Thermostat.process.json", import.meta.url),
    "utf8")).processes[0].graph;
const environment = JSON.parse(readFileSync(new URL("environments/thermostat.json", import.meta.url), "utf8"));

test.use({graphs: [], environment, pictures: devicePictures()});

test("building the thermostat", async ({page, engine}) => {
    test.setTimeout(120000);
    await lightTheme(page);
    await offline(page);
    await loggedIn(page, engine.url);
    const editor = page.getByTestId("editor");
    const inspector = page.getByTestId("inspector");
    const field = (port: string) => inspector.getByTestId("input-value-" + port).getByTestId("value-field");
    const row = (id: string, side: "input" | "output", port: string) =>
        block(page, id).getByTestId(side + "-" + port).getByTestId(side + "-title");
    const emptyCanvas = async () => {
        const box = (await editor.boundingBox())!;
        return {x: 30, y: box.height - 30};
    };
    const zoomToFit = async () => menu(page, editor, "zoom-fit", await emptyCanvas());

    // 4.1 Creating the process.
    await page.getByTestId("process-new").click();
    await page.fill("#newProcessName", example.name);
    await page.fill("#newProcessDescription", example.description);
    await shot(page, "thermostat-new");
    await page.getByTestId("dialog-ok").click();
    await expect(processItem(page, "Thermostat")).toHaveClass(/active/);
    const G = (await processItem(page, "Thermostat").getAttribute("data-id"))!;
    const other = await engine.client();

    // 4.2 The block: found in the palette, dragged onto the canvas, renamed.
    await page.getByTestId("palette-search").fill("hyst");
    const box = (await editor.boundingBox())!;
    await page.locator("[data-testid=block-type][data-type='process/Hysteresis']")
        .dragTo(editor, {targetPosition: {x: box.width / 2 - 90, y: 120}});
    await block(page, "Hysteresis_1").getByTestId("title").click();
    await mark(page, [
        {at: page.getByTestId("palette-search"), n: 1},
        {at: block(page, "Hysteresis_1"), n: 2},
        {at: page.getByTestId("inspector-name"), n: 3},
    ]);
    await shot(page, "thermostat-block");
    await page.locator(".manual-mark").evaluateAll(marks => marks.forEach(m => m.remove()));
    await page.getByTestId("inspector-name").fill("heating");
    await page.getByTestId("inspector-name").press("Enter");
    await expect(block(page, "heating")).toHaveCount(1);
    await page.getByTestId("palette-search").fill("");

    // 4.3 Its fixed values, in the inspector.
    for (const [port, value] of [["threshold", "21"], ["hysteresis", "0.5"]]) {
        await field(port).fill(value);
        await field(port).press("Enter");
        await expect(inspector.getByTestId("input-value-" + port).getByTestId("input-state")).toHaveText("Fixed value");
    }
    await inspector.getByTestId("input-value-inverted").getByTestId("value-switch").click();
    await expect(page.getByTestId("value-node")).toHaveCount(3);
    await shot(page, "thermostat-values");

    // 4.4 Process ports, bound to the I/O.
    await menu(page, row("heating", "input", "input"), "bind");
    await page.getByTestId("bind-port-name").fill("room_temperature");
    await page.getByTestId("bind-channel").selectOption("slot1-ai[1]");
    await shot(page, "thermostat-bind", page.locator(".hk-dialog, [role=dialog]").first());
    await page.getByTestId("dialog-ok").click();
    await expect(page.getByTestId("bind-channel")).toHaveCount(0);
    await menu(page, row("heating", "output", "output"), "bind");
    await page.getByTestId("bind-port-name").fill("heater");
    await page.getByTestId("bind-channel").selectOption("slot2-dq");
    await page.getByTestId("dialog-ok").click();
    await expect(page.getByTestId("bind-channel")).toHaveCount(0);
    await page.keyboard.press("Escape");
    await zoomToFit();
    await shotOfProcess(page, "thermostat-ports");

    // 4.5 Started, with the room's temperature watched and the device window open.
    await processItem(page, "Thermostat").getByTestId("process-run").click();
    await expect(processItem(page, "Thermostat").getByTestId("process-state")).toHaveClass(/running/);
    engine.environmentSend({value: {port: "slot1-ai", index: 1, value: 19.6}});
    await block(page, "heating").getByTestId("title").click();
    await inspector.locator("[data-testid=inspector-port][data-port=input]").getByTestId("port-watch").click();
    await expect(inspector.getByTestId("port-live")).toContainText("19.6");
    await mark(page, [
        {at: processItem(page, "Thermostat").getByTestId("process-run"), n: 1, side: "top"},
        {at: inspector.locator("[data-testid=inspector-port][data-port=input]").getByTestId("port-watch"), n: 2, side: "top"},
        {at: inspector.getByTestId("port-live"), n: 3, side: "right"},
    ]);
    await shot(page, "thermostat-running");
    await page.locator(".manual-mark").evaluateAll(marks => marks.forEach(m => m.remove()));
    await page.getByTestId("env-device").click();
    await expect(page.getByTestId("device-window")).toBeVisible();
    await page.waitForTimeout(1000);      // the pictures
    await shot(page, "thermostat-device", page.getByTestId("device-window"));
    await page.getByTestId("env-device").click();

    // 4.6 The setpoint changed while it runs: a double-click on its value.
    await page.keyboard.press("Escape");
    const threshold = page.getByTestId("value-node").filter({hasText: "21"});
    await threshold.dblclick();
    await page.keyboard.press("ControlOrMeta+a");
    await page.keyboard.type("22");
    await shotOfProcess(page, "thermostat-setpoint");
    await page.keyboard.press("Enter");
    await expect(page.getByTestId("value-node").filter({hasText: "22"})).toHaveCount(1);

    // 4.7 Unsaved, then saved.
    await expect(processItem(page, "Thermostat").getByTestId("process-unsaved")).toBeVisible();
    await shot(page, "thermostat-unsaved", page.locator(".hk-panel").filter({has: page.getByTestId("process-new")}));
    await page.keyboard.press("ControlOrMeta+s");
    await expect(processItem(page, "Thermostat").getByTestId("process-unsaved")).toHaveCount(0);

    // 4.8 The schedule: its blocks as the chapter lists them, the last connection drawn here. A
    // wider window, for the wider process.
    await page.setViewportSize({width: 1800, height: 860});
    await other.connectGraph(G);
    const blocks = example.blocks as Record<string, { component: string, metadata: object }>;
    for (const [id, b] of Object.entries(blocks)) {
        if (id === "heating")
            await other.command("graph", "changenode", {graph: G, id, metadata: b.metadata});
        else
            await other.command("graph", "addnode", {graph: G, id, component: b.component, metadata: b.metadata});
    }
    type End = { block?: string, port: string, data?: unknown };
    for (const c of example.connections as { src: End & { data?: unknown }, tgt: End }[]) {
        if (c.tgt.block === "heating")
            continue;
        if ("data" in c.src)
            await other.command("graph", "addinitial", {graph: G, src: {data: c.src.data},
                tgt: {node: c.tgt.block, port: c.tgt.port}});
        else
            await other.command("graph", "addedge", {graph: G, src: {node: c.src.block, port: c.src.port},
                tgt: {node: c.tgt.block, port: c.tgt.port}});
    }
    await zoomToFit();
    await expect(block(page, "setpoint")).toBeVisible();
    const from = block(page, "setpoint").getByTestId("output-output").getByTestId("output-socket");
    const to = block(page, "heating").getByTestId("input-threshold").getByTestId("input-socket");
    await from.hover();
    await page.mouse.down();
    await to.hover();
    await page.mouse.up();
    await expect(page.getByTestId("value-node").filter({hasText: "22"})).toHaveCount(0);
    await menu(page, editor, "watch-all", await emptyCanvas());
    await zoomToFit();
    await expect(page.getByTestId("connection")).toHaveCount(4);
    await shotOfProcess(page, "thermostat-schedule");
});
