import type {Page} from "@playwright/test";
import {expect, test, VIEWER} from "./fixtures";
import {EXAMPLE_GRAPH} from "./paths";
import {block, blockIds, blockPosition, blocks, connections, inputLabels, loggedIn, openProcess, selectedIds} from "./ui";

// 06 §4.10: copy, paste and duplicate — the selection with its inner connections and fixed
// values, pasted as commands; each part appears with its event.

const G = "0826152e-3014-4c89-9380-ec4991a76af2";

async function openExample(page: Page, url: string) {
    await loggedIn(page, url);
    await openProcess(page, "Simple Example");
    await expect(blocks(page)).toHaveCount(8);
}

const title = (page: Page, id: string) => block(page, id).getByTestId("title");
const item = (page: Page, action: string) => page.getByTestId("context-menu").locator(`[data-action="${action}"]`);

test("Ctrl+C and Ctrl+V paste the selection beside it, again a step further", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    await other.connectGraph(G);
    await title(page, "block1").click();
    await title(page, "block3").click({modifiers: ["Shift"]});
    await page.keyboard.press("Control+c");
    await page.keyboard.press("Control+v");

    const added = [(await other.next(m => m.command === "addnode")).payload, (await other.next(m => m.command === "addnode")).payload];
    expect(added.map(p => p.id).sort()).toEqual(["block1_1", "block3_1"]);
    expect((await other.next(m => m.command === "addedge")).payload)
        .toMatchObject({src: {node: "block1_1", port: "out"}, tgt: {node: "block3_1", port: "in1"}});
    await expect(blocks(page)).toHaveCount(10);
    await expect(connections(page)).toHaveCount(7);
    const p1 = await blockPosition(page, "block1"), c1 = await blockPosition(page, "block1_1");
    expect([c1.x - p1.x, c1.y - p1.y]).toEqual([40, 40]);
    // The pasted blocks are the selection.
    await expect.poll(() => selectedIds(page)).toEqual(["block1_1", "block3_1"]);

    await page.keyboard.press("Control+v");
    await expect(blocks(page)).toHaveCount(12);
    const c2 = await blockPosition(page, "block1_2");
    expect([c2.x - p1.x, c2.y - p1.y]).toEqual([80, 80]);
});

test("Ctrl+X cuts the selection, which pastes back; the cut is one undo step", async ({page, engine}) => {
    await openExample(page, engine.url);
    await title(page, "block1").click();
    await title(page, "block3").click({modifiers: ["Shift"]});
    await page.keyboard.press("Control+x");
    await expect(blocks(page)).toHaveCount(6);
    await expect(block(page, "block1")).toHaveCount(0);
    // Pasted: the same ids again (they are free), with their connection.
    await page.keyboard.press("Control+v");
    await expect(blocks(page)).toHaveCount(8);
    await expect(block(page, "block1")).toHaveCount(1);
    await expect(block(page, "block3")).toHaveCount(1);
    await expect.poll(() => selectedIds(page)).toEqual(["block1", "block3"]);
    // Undo: the paste, then the cut as one step (both blocks and their connections back).
    await page.keyboard.press("Control+z");
    await expect(blocks(page)).toHaveCount(6);
    await page.keyboard.press("Control+z");
    await expect(blocks(page)).toHaveCount(8);
    await expect(connections(page)).toHaveCount(6);

    // The menu's Cut: the block alone when it is not in the selection.
    await page.keyboard.press("Escape");
    await title(page, "block7").click({button: "right"});
    await item(page, "cut").click();
    await expect(block(page, "block7")).toHaveCount(0);
    await title(page, "block8").click({button: "right"});
    await expect(item(page, "cut")).toHaveText(/Cut/);
});

// A clip copied in another browser reaches this one through the system clipboard as plain text
// (JSON, 06 §4.10): what any browser's paste event then carries.
test("a clip copied elsewhere, as text on the clipboard, pastes", async ({page, engine}) => {
    await openExample(page, engine.url);
    const clip = {format: "hakkas/blocks", version: 1, links: [],
        blocks: [{id: "elsewhere", component: "math/Add2", x: 100, y: 100, initials: {in2: "5"}}]};
    // Onto the system clipboard as text, as another browser's copy leaves it: from a field.
    await page.evaluate(text => {
        const field = document.createElement("textarea");
        field.id = "elsewhere";
        field.value = text;
        document.body.append(field);
        field.select();
    }, JSON.stringify(clip));
    await page.keyboard.press("Control+c");
    await page.evaluate(() => document.getElementById("elsewhere")?.remove());
    await page.keyboard.press("Control+v");
    await expect(block(page, "elsewhere")).toHaveCount(1);
    await expect.poll(() => inputLabels(page, "elsewhere")).toEqual(["in1", "in2 = 5"]);
});

test.describe("as a viewer", () => {
    test("Ctrl+X does not cut, and the menu has no Cut", async ({page, engine}) => {
        await loggedIn(page, engine.url, VIEWER);
        await openProcess(page, "Simple Example");
        await expect(blocks(page)).toHaveCount(8);
        await title(page, "block1").click();
        await page.keyboard.press("Control+x");
        await title(page, "block2").click({button: "right"});
        await expect(item(page, "copy")).toHaveCount(1);
        await expect(item(page, "cut")).toHaveCount(0);
        await expect(blocks(page)).toHaveCount(8);
    });
});

test("fixed values come along; Ctrl+D and the menu duplicate", async ({page, engine}) => {
    await openExample(page, engine.url);
    await title(page, "block8").click();
    await page.keyboard.press("Control+d");
    await expect(block(page, "block8_1")).toHaveCount(1);
    await expect.poll(() => inputLabels(page, "block8_1")).toEqual(["in1 = 12", "in2 = 23"]);

    await title(page, "block7").click({button: "right"});
    await item(page, "duplicate").click();
    await expect(block(page, "block7_1")).toHaveCount(1);
    expect(await blockIds(page)).toHaveLength(10);
});

test.describe("with two processes", () => {
    test.use({graphs: [{id: "second", name: "Second", connections: [], blocks: {}}, EXAMPLE_GRAPH]});

    test("copied in one process, pasted into another where the menu was opened", async ({page, engine}) => {
        await openExample(page, engine.url);
        const other = await engine.client();
        await other.connectGraph("second");
        await title(page, "block8").click({button: "right"});
        await item(page, "copy").click();

        await openProcess(page, "Second");
        const editor = (await page.getByTestId("editor").boundingBox())!;
        await page.mouse.click(editor.x + 200, editor.y + 150, {button: "right"});
        await item(page, "paste").click();
        // The name is free there, so it is kept.
        expect((await other.next(m => m.command === "addnode")).payload).toMatchObject({graph: "second", id: "block8"});
        await expect(block(page, "block8")).toHaveCount(1);
        const box = (await block(page, "block8").boundingBox())!;
        expect(Math.abs(box.x - (editor.x + 200))).toBeLessThan(2);
        await expect.poll(() => inputLabels(page, "block8")).toEqual(["in1 = 12", "in2 = 23"]);
    });
});
