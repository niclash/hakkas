import {Page} from "@playwright/test";
import {expect, test} from "./fixtures";
import {block, blockIds, blockPosition, blocks, connections, inputLabels, loggedIn, openProcess} from "./ui";

// 06 §4.13: undo and redo of this tab's own edits, sent as inverse commands; a step changed by
// someone else is skipped; the history survives a reload of the tab.

const G = "0826152e-3014-4c89-9380-ec4991a76af2";

async function openExample(page: Page, url: string) {
    await loggedIn(page, url);
    await openProcess(page, "Simple Example");
    await expect(blocks(page)).toHaveCount(8);
}

const title = (page: Page, id: string) => block(page, id).getByTestId("title");

test("a deleted block comes back with its connections, and goes again on redo", async ({page, engine}) => {
    await openExample(page, engine.url);
    const at = await blockPosition(page, "block7");
    await title(page, "block7").click();
    await page.keyboard.press("Delete");
    await expect(blocks(page)).toHaveCount(7);
    await expect(connections(page)).toHaveCount(4);

    await page.keyboard.press("Control+z");
    await expect(blocks(page)).toHaveCount(8);
    await expect(connections(page)).toHaveCount(6);
    expect(await blockPosition(page, "block7")).toEqual(at);

    await page.keyboard.press("Control+Shift+z");
    await expect(blocks(page)).toHaveCount(7);
    await page.keyboard.press("Control+y");   // nothing more to redo
    await page.keyboard.press("Control+z");
    await expect(blocks(page)).toHaveCount(8);
});

test("a drag of two blocks is one step; quick undos follow each other", async ({page, engine}) => {
    await openExample(page, engine.url);
    const p7 = await blockPosition(page, "block7"), p8 = await blockPosition(page, "block8");
    await title(page, "block8").click();
    await title(page, "block7").click({modifiers: ["Shift"]});
    const box = (await title(page, "block8").boundingBox())!;
    await page.mouse.move(box.x + 10, box.y + 5);
    await page.mouse.down();
    await page.mouse.move(box.x + 90, box.y + 70, {steps: 8});
    await page.mouse.up();
    await expect.poll(async () => (await blockPosition(page, "block8")).x).toBeGreaterThan(p8.x);

    // Two blocks added from the palette: two more steps.
    await page.locator("[data-type='math/Add2']").click();
    await page.locator("[data-type='math/Add2']").click();
    await expect(blocks(page)).toHaveCount(10);

    for (let i = 0; i < 3; i++)
        await page.keyboard.press("Control+z");
    await expect(blocks(page)).toHaveCount(8);
    await expect.poll(() => blockPosition(page, "block8")).toEqual(p8);
    expect(await blockPosition(page, "block7")).toEqual(p7);
});

test("values, renames and pastes are undone; the history survives a reload", async ({page, engine}) => {
    await openExample(page, engine.url);
    await title(page, "block7").click();
    const field = page.getByTestId("input-value-in3").getByTestId("value-field");
    await field.fill("4");
    await field.press("Enter");
    await expect.poll(() => inputLabels(page, "block7")).toEqual(["in1", "in2", "in3 = 4", "in4"]);
    await page.getByTestId("inspector-name").fill("sum");
    await page.getByTestId("inspector-name").press("Enter");
    await expect(block(page, "sum")).toHaveCount(1);
    await title(page, "block8").click();
    await page.keyboard.press("Control+c");
    await page.keyboard.press("Control+v");
    await expect(block(page, "block8_1")).toHaveCount(1);

    await page.reload();
    await expect(blocks(page)).toHaveCount(9);
    await expect(page.getByTestId("editor")).toHaveAttribute("data-settled", "true");
    await page.keyboard.press("Control+z");
    await expect(block(page, "block8_1")).toHaveCount(0);
    await page.keyboard.press("Control+z");
    await expect(block(page, "block7")).toHaveCount(1);
    await page.keyboard.press("Control+z");
    await expect.poll(() => inputLabels(page, "block7")).toEqual(["in1", "in2", "in3", "in4"]);
    expect(await blockIds(page)).toHaveLength(8);
});

test("a step someone else has changed since is skipped, with a message", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    await page.locator("[data-type='math/Add2']").click();       // step 1: an added block
    await expect(block(page, "Add2_1")).toHaveCount(1);
    await title(page, "block8").click();
    await page.keyboard.press("Delete");                           // step 2: a deletion
    await expect(blocks(page)).toHaveCount(8);
    // Someone else adds a block8 again.
    await other.command("graph", "addnode", {graph: G, id: "block8", component: "math/Mult2", metadata: {x: 0, y: 0}});
    await expect(blocks(page)).toHaveCount(9);

    await page.keyboard.press("Control+z");
    await expect(page.getByTestId("notice")).toHaveText("Not undone: changed since by someone else");
    await expect(blocks(page)).toHaveCount(9);
    // The next undo goes on with the step before.
    await page.keyboard.press("Control+z");
    await expect(block(page, "Add2_1")).toHaveCount(0);
});

test("Ctrl+Z in a field is the field's", async ({page, engine}) => {
    await openExample(page, engine.url);
    await page.locator("[data-type='math/Add2']").click();
    await expect(blocks(page)).toHaveCount(9);
    await title(page, "block7").click();
    await page.getByTestId("inspector-name").click();
    await page.keyboard.press("Control+z");
    await page.waitForTimeout(200);
    await expect(blocks(page)).toHaveCount(9);
});
