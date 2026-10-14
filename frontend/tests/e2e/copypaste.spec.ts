import {Page} from "@playwright/test";
import {expect, test} from "./fixtures";
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
