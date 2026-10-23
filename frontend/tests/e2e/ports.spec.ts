import type {Page} from "@playwright/test";
import {expect, test} from "./fixtures";
import {block, blocks, loggedIn, openProcess, selectedIds} from "./ui";

// 06 §4.8: process ports, exposed from a block port's menu, drawn as markers at the process's
// edge, renamed and removed through the engine.

const G = "0826152e-3014-4c89-9380-ec4991a76af2";

async function openExample(page: Page, url: string) {
    await loggedIn(page, url);
    await openProcess(page, "Simple Example");
    await expect(blocks(page)).toHaveCount(8);
}

const item = (page: Page, action: string) => page.getByTestId("context-menu").locator(`[data-action="${action}"]`);
const marker = (page: Page, direction: string, name: string) =>
    page.locator(`[data-testid=port-node][data-direction=${direction}][data-name="${name}"]`);
const row = (page: Page, id: string, side: "input" | "output", port: string) =>
    block(page, id).getByTestId(side + "-" + port).getByTestId(side + "-title");

async function edges(page: Page): Promise<{ left: number, right: number }> {
    const boxes = await Promise.all((await blocks(page).all()).map(b => b.boundingBox()));
    return {left: Math.min(...boxes.map(b => b!.x)), right: Math.max(...boxes.map(b => b!.x + b!.width))};
}

async function levelWith(page: Page, name: string, direction: string, id: string, side: "input" | "output", port: string) {
    const m = (await marker(page, direction, name).boundingBox())!;
    const s = (await block(page, id).getByTestId(side + "-" + port).getByTestId(side + "-socket").boundingBox())!;
    return Math.abs((m.y + m.height / 2) - (s.y + s.height / 2)) < 2;
}

test("expose an input and an output; markers at the process's edges", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    await other.connectGraph(G);

    await row(page, "block7", "input", "in3").click({button: "right"});
    await item(page, "expose-input").click();
    const field = page.getByTestId("port-name-field");
    await expect(field).toHaveValue("in3");
    await field.fill("extra");
    await field.press("Enter");
    expect((await other.next(m => m.command === "addinport")).payload)
        .toMatchObject({graph: G, name: "extra", node: "block7", port: "in3"});
    await expect(page.getByTestId("port-name-field")).toHaveCount(0);
    await expect(marker(page, "in", "extra")).toHaveAttribute("data-target", "block7.in3");
    expect((await marker(page, "in", "extra").boundingBox())!.x).toBeLessThan((await edges(page)).left);
    expect(await levelWith(page, "extra", "in", "block7", "input", "in3")).toBe(true);
    await expect(page.getByTestId("port-connection")).toHaveCount(1);

    await row(page, "block7", "output", "out").click({button: "right"});
    await item(page, "expose-output").click();
    await expect(page.getByTestId("port-name-field")).toHaveValue("out");
    await page.getByTestId("dialog-ok").click();
    await expect(marker(page, "out", "out")).toHaveAttribute("data-target", "block7.out");
    const out = (await marker(page, "out", "out").boundingBox())!;
    expect(out.x).toBeGreaterThan((await edges(page)).right);

    // Markers are not blocks.
    await page.keyboard.press("Control+a");
    expect(await selectedIds(page)).toHaveLength(8);
});

test("a name already taken is refused in the dialog", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    await other.command("graph", "addinport", {graph: G, node: "block1", port: "in1", public: "flow"});
    await expect(marker(page, "in", "flow")).toHaveCount(1);
    await row(page, "block2", "input", "in1").click({button: "right"});
    await item(page, "expose-input").click();
    await page.getByTestId("port-name-field").fill("flow");
    await page.getByTestId("port-name-field").press("Enter");
    await expect(page.getByTestId("port-name-error")).toHaveText("Port exists: flow");
    await expect(page.getByTestId("error")).toHaveCount(0);
    await page.getByTestId("dialog-cancel").click();
    await expect(page.getByTestId("port-name-field")).toHaveCount(0);
    await expect(page.getByTestId("port-node")).toHaveCount(1);
});

test("rename and remove a process port from its marker's menu", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    await other.connectGraph(G);
    await other.command("graph", "addoutport", {graph: G, node: "block7", port: "out", public: "total"});
    await marker(page, "out", "total").click({button: "right"});
    await item(page, "rename-port").click();
    await expect(page.getByTestId("port-name-field")).toHaveValue("total");
    await page.getByTestId("port-name-field").fill("sum");
    await page.getByTestId("dialog-ok").click();
    expect((await other.next(m => m.command === "renameoutport")).payload).toMatchObject({from: "total", to: "sum"});
    await expect(marker(page, "out", "sum")).toHaveCount(1);
    await expect(marker(page, "out", "total")).toHaveCount(0);

    await marker(page, "out", "sum").click({button: "right"});
    await item(page, "remove-port").click();
    expect((await other.next(m => m.command === "removeoutport")).payload).toMatchObject({name: "sum"});
    await expect(page.getByTestId("port-node")).toHaveCount(0);
    await expect(page.getByTestId("port-connection")).toHaveCount(0);
});

test("markers follow their block: moved, renamed, removed", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    await other.command("graph", "addinport", {graph: G, node: "block8", port: "in2", public: "b"});
    await expect(marker(page, "in", "b")).toHaveCount(1);
    await other.command("graph", "changenode", {graph: G, id: "block8", metadata: {x: 278, y: 900}});
    await expect.poll(() => levelWith(page, "b", "in", "block8", "input", "in2")).toBe(true);
    await other.command("graph", "renamenode", {graph: G, from: "block8", to: "adder"});
    await expect(marker(page, "in", "b")).toHaveAttribute("data-target", "adder.in2");
    await other.command("graph", "removenode", {graph: G, id: "adder"});
    await expect(page.getByTestId("port-node")).toHaveCount(0);
});
