import type {Page} from "@playwright/test";
import {expect, test} from "./fixtures";
import {EXAMPLE_GRAPH} from "./paths";
import {block, blocks, connections, loggedIn, openProcess, processItem} from "./ui";

// 06 §4.7: live values. The watched inputs are sent as network:edges (the whole set each time);
// network:data shows on the connection and in the inspector.

const G = "0826152e-3014-4c89-9380-ec4991a76af2";

async function openExample(page: Page, url: string) {
    await loggedIn(page, url);
    await openProcess(page, "Simple Example");
    await expect(blocks(page)).toHaveCount(8);
}

const menu = (page: Page) => page.getByTestId("context-menu");
const item = (page: Page, action: string) => menu(page).locator(`[data-action="${action}"]`);
const label = (page: Page, target: string) => page.locator(`[data-testid=live-value][data-target="${target}"]`);
const inputRow = (page: Page, id: string, port: string) => block(page, id).getByTestId("input-" + port).getByTestId("input-title");

async function emptyCanvas(page: Page): Promise<{ x: number, y: number }> {
    const box = (await block(page, "block1").boundingBox())!;
    return {x: box.x - 10, y: box.y - 10};
}

test("a watched connection shows its value as it changes", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    await inputRow(page, "block7", "in1").click({button: "right"});
    await item(page, "watch").click();
    await expect(label(page, "block7.in1")).toHaveText("0");

    // block1.in1 = 5 runs through block1 and block3 into block7.in1.
    other.send("network", "start", {graph: G});
    await other.command("graph", "addinitial", {graph: G, tgt: {node: "block1", port: "in1"}, src: {data: 5}});
    await expect(label(page, "block7.in1")).toHaveText("5");
    await other.command("graph", "addinitial", {graph: G, tgt: {node: "block2", port: "in1"}, src: {data: 2.25}});
    await expect(label(page, "block7.in1")).toHaveText("7.25");

    // The inspector shows it too, and its toggle stops the watch.
    await block(page, "block7").getByTestId("title").click();
    const in1 = page.getByTestId("inspector").locator("[data-port=in1]");
    await expect(in1.getByTestId("port-live")).toHaveText("Live: 7.25");
    await in1.getByTestId("port-watch").click();
    await expect(label(page, "block7.in1")).toHaveCount(0);
    await expect(in1.getByTestId("port-live")).toHaveCount(0);
});

test("a block's inputs, also those with a fixed value; the menu stops them", async ({page, engine}) => {
    await openExample(page, engine.url);
    await block(page, "block8").getByTestId("title").click({button: "right"});
    await item(page, "watch-inputs").click();
    await expect(label(page, "block8.in1")).toHaveText("12");
    await expect(label(page, "block8.in2")).toHaveText("23");
    await block(page, "block8").getByTestId("title").click({button: "right"});
    await item(page, "unwatch-inputs").click();
    await expect(page.getByTestId("live-value")).toHaveCount(0);
});

test("watch all connections, also one drawn later; stopping one stops them all", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    const at = await emptyCanvas(page);
    await page.mouse.click(at.x, at.y, {button: "right"});
    await item(page, "watch-all").click();
    await expect(page.getByTestId("live-value")).toHaveCount(await connections(page).count());
    // block8.out is 12 + 23 once the process runs.
    other.send("network", "start", {graph: G});
    await other.command("graph", "addedge", {graph: G, src: {node: "block8", port: "out"}, tgt: {node: "block7", port: "in3"}});
    await expect(label(page, "block7.in3")).toHaveText("35");
    await expect(page.getByTestId("live-value")).toHaveCount(7);

    await connections(page).first().locator("path").click({button: "right"});
    await expect(item(page, "unwatch")).toBeVisible();
    await item(page, "unwatch").click();
    await expect(page.getByTestId("live-value")).toHaveCount(0);
});

test("a renamed block keeps its watches", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    await inputRow(page, "block7", "in2").click({button: "right"});
    await item(page, "watch").click();
    await expect(label(page, "block7.in2")).toHaveText("0");
    await other.command("graph", "renamenode", {graph: G, from: "block7", to: "sum"});
    await expect(label(page, "sum.in2")).toHaveText("0");
});

test.describe("with two processes", () => {
    test.use({graphs: [{id: "second", name: "Second", connections: [], blocks: {}}, EXAMPLE_GRAPH]});

    test("opening another process clears the watches", async ({page, engine}) => {
        await openExample(page, engine.url);
        await inputRow(page, "block7", "in1").click({button: "right"});
        await item(page, "watch").click();
        await expect(label(page, "block7.in1")).toHaveCount(1);
        await openProcess(page, "Second");
        await openProcess(page, "Simple Example");
        await expect(blocks(page)).toHaveCount(8);
        await expect(page.getByTestId("live-value")).toHaveCount(0);
        expect(processItem(page, "Second")).toBeTruthy();
    });
});
