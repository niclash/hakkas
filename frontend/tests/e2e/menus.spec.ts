import {Page} from "@playwright/test";
import {expect, test} from "./fixtures";
import {block, blockPosition, blocks, connections, inputLabels, loggedIn, openProcess, selectedIds, valueNode} from "./ui";

// 06 §4.4: the editor's own context menus. Every item that changes the process sends commands;
// the view changes with the engine's events.

const G = "0826152e-3014-4c89-9380-ec4991a76af2";

async function openExample(page: Page, url: string) {
    await loggedIn(page, url);
    await openProcess(page, "Simple Example");
    await expect(blocks(page)).toHaveCount(8);
}

const menu = (page: Page) => page.getByTestId("context-menu");
const item = (page: Page, action: string) => menu(page).locator(`[data-action="${action}"]`);
const actions = (page: Page) => menu(page).getByTestId("menu-item").evaluateAll(e => e.map(i => i.getAttribute("data-action")));
const title = (page: Page, id: string) => block(page, id).getByTestId("title");
const inputRow = (page: Page, id: string, port: string) => block(page, id).getByTestId("input-" + port).getByTestId("input-title");

// A point on the empty canvas, above and left of block1 (the top-left block).
async function emptyCanvas(page: Page): Promise<{ x: number, y: number }> {
    const box = (await block(page, "block1").boundingBox())!;
    return {x: box.x - 10, y: box.y - 10};
}

test("the canvas menu, by keyboard: arrows, Escape", async ({page, engine}) => {
    await openExample(page, engine.url);
    const at = await emptyCanvas(page);
    await page.mouse.click(at.x, at.y, {button: "right"});
    expect(await actions(page)).toEqual(["add-block", "select-all", "zoom-fit", "arrange", "watch-all"]);
    await expect(item(page, "add-block")).toBeFocused();
    await page.keyboard.press("ArrowDown");
    await expect(item(page, "select-all")).toBeFocused();
    await page.keyboard.press("ArrowUp");
    await page.keyboard.press("ArrowUp");
    // Up from the first item wraps to the last.
    await expect(item(page, "watch-all")).toBeFocused();
    await page.keyboard.press("Escape");
    await expect(menu(page)).toHaveCount(0);

    await page.mouse.click(at.x, at.y, {button: "right"});
    await page.keyboard.press("ArrowDown");
    await page.keyboard.press("Enter");
    await expect(menu(page)).toHaveCount(0);
    expect(await selectedIds(page)).toHaveLength(8);
    // A click on the canvas (beside the menu) closes a menu.
    await page.mouse.click(at.x, at.y, {button: "right"});
    const open = (await menu(page).boundingBox())!;
    await page.mouse.click(open.x + open.width + 30, at.y);
    await expect(menu(page)).toHaveCount(0);
});

test("Add block… searches the types and adds one where the menu was opened", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    await other.connectGraph(G);
    const at = await emptyCanvas(page);
    await page.mouse.click(at.x, at.y, {button: "right"});
    await item(page, "add-block").click();
    await expect(page.getByTestId("block-search")).toBeFocused();
    await page.keyboard.type("mult");
    await expect(page.getByTestId("block-result")).toHaveText(["math/Mult2"]);
    await page.keyboard.type("zz");
    await expect(page.getByTestId("block-none")).toBeVisible();
    await page.keyboard.press("Backspace");
    await page.keyboard.press("Backspace");
    await page.keyboard.press("Enter");
    expect((await other.next(m => m.command === "addnode")).payload).toMatchObject({id: "Mult2_1", component: "math/Mult2"});
    await expect(menu(page)).toHaveCount(0);
    const box = (await block(page, "Mult2_1").boundingBox())!;
    expect(Math.abs(box.x - at.x)).toBeLessThan(2);
    expect(Math.abs(box.y - at.y)).toBeLessThan(2);
});

test("a block's menu: rename, edit inputs, delete it or the selection", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    await other.connectGraph(G);

    await title(page, "block7").click({button: "right"});
    expect(await actions(page)).toEqual(["rename", "edit-inputs", "watch-inputs", "copy", "duplicate", "create-composite", "delete-block"]);
    await item(page, "edit-inputs").click();
    expect(await selectedIds(page)).toEqual(["block7"]);
    // The first input that takes a value (in1 and in2 are connected).
    await expect(page.getByTestId("input-value-in3").getByTestId("value-field")).toBeFocused();

    await title(page, "block1").click({button: "right"});
    await item(page, "delete-block").click();
    expect((await other.next(m => m.command === "removenode")).payload.id).toBe("block1");
    await expect(blocks(page)).toHaveCount(7);

    await title(page, "block2").click();
    await title(page, "block4").click({modifiers: ["Shift"]});
    await title(page, "block4").click({button: "right"});
    await expect(item(page, "delete-selection")).toHaveText(/Delete 2 blocks/);
    await item(page, "delete-selection").click();
    await expect(blocks(page)).toHaveCount(5);
});

test("an input's menu: set, change and clear a fixed value, or disconnect", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    await other.connectGraph(G);

    await inputRow(page, "block7", "in3").click({button: "right"});
    expect(await actions(page)).toEqual(["set-value", "watch", "expose-input"]);
    await item(page, "set-value").click();
    await page.getByTestId("value-draft-field").fill("2");
    await page.getByTestId("value-draft-field").press("Enter");
    await expect.poll(() => inputLabels(page, "block7")).toEqual(["in1", "in2", "in3 = 2", "in4"]);

    await inputRow(page, "block8", "in1").click({button: "right"});
    expect(await actions(page)).toEqual(["change-value", "clear-value", "watch", "expose-input"]);
    await item(page, "change-value").click();
    const field = valueNode(page, "block8", "in1").getByTestId("value-node-field");
    await expect(field).toBeFocused();
    await field.fill("13");
    await field.press("Enter");
    await expect.poll(() => inputLabels(page, "block8")).toEqual(["in1 = 13", "in2 = 23"]);

    await valueNode(page, "block8", "in2").click({button: "right"});
    expect(await actions(page)).toEqual(["change-value", "clear-value", "watch"]);
    await item(page, "clear-value").click();
    expect((await other.next(m => m.command === "removeinitial")).payload).toMatchObject({tgt: {node: "block8", port: "in2"}});
    await expect.poll(() => inputLabels(page, "block8")).toEqual(["in1 = 13", "in2"]);

    await inputRow(page, "block7", "in1").click({button: "right"});
    expect(await actions(page)).toEqual(["disconnect", "watch", "expose-input"]);
    await item(page, "disconnect").click();
    expect((await other.next(m => m.command === "removeedge")).payload)
        .toMatchObject({src: {node: "block3", port: "out"}, tgt: {node: "block7", port: "in1"}});
    await expect(connections(page)).toHaveCount(5);
});

test("a connection's menu deletes it", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    await other.connectGraph(G);
    await connections(page).first().locator("path").click({button: "right"});
    expect(await actions(page)).toEqual(["delete-connection", "watch"]);
    await item(page, "delete-connection").click();
    await other.next(m => m.command === "removeedge");
    await expect(connections(page)).toHaveCount(5);
});

test("Arrange blocks moves the blocks through the engine", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    await other.connectGraph(G);
    const before = await blockPosition(page, "block7");
    const at = await emptyCanvas(page);
    await page.mouse.click(at.x, at.y, {button: "right"});
    await item(page, "arrange").click();
    const moved = await other.next(m => m.command === "changenode");
    expect(moved.payload.metadata).toEqual({x: expect.any(Number), y: expect.any(Number)});
    await expect.poll(async () => JSON.stringify(await blockPosition(page, "block7"))).not.toBe(JSON.stringify(before));
});

test("the menu key opens the selected block's menu", async ({page, engine}) => {
    await openExample(page, engine.url);
    await title(page, "block8").click();
    await page.keyboard.press("Shift+F10");
    await expect(item(page, "rename")).toBeFocused();
    await page.keyboard.press("Enter");
    await expect(page.getByTestId("inspector-name")).toBeFocused();
});

test("a menu opened near the edge stays inside the editor", async ({page, engine}) => {
    await openExample(page, engine.url);
    const editor = (await page.getByTestId("editor").boundingBox())!;
    await page.mouse.click(editor.x + editor.width - 5, editor.y + 10, {button: "right"});
    await item(page, "add-block").click();
    const box = (await menu(page).boundingBox())!;
    expect(box.x + box.width).toBeLessThanOrEqual(editor.x + editor.width);
});
