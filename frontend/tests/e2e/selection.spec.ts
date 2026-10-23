import type {Page} from "@playwright/test";
import {expect, test} from "./fixtures";
import {block, blockCentre, blockPosition, blocks, loggedIn, openProcess, selectedIds} from "./ui";

// 06 §4: selecting blocks is the view's own state; what is done to the selection goes to the
// engine as commands, and the view changes with the events.

const G = "0826152e-3014-4c89-9380-ec4991a76af2";

async function openExample(page: Page, url: string) {
    await loggedIn(page, url);
    await openProcess(page, "Simple Example");
    await expect(blocks(page)).toHaveCount(8);
}

function title(page: Page, id: string) {
    return block(page, id).getByTestId("title");
}

// A point on the empty canvas, above and left of block1 (the top-left block).
async function emptyCanvas(page: Page): Promise<{ x: number, y: number }> {
    const box = (await block(page, "block1").boundingBox())!;
    return {x: box.x - 10, y: box.y - 10};
}

test("clicks select blocks, Escape and the canvas clear the selection", async ({page, engine}) => {
    await openExample(page, engine.url);
    await title(page, "block1").click();
    expect(await selectedIds(page)).toEqual(["block1"]);
    await title(page, "block2").click();
    expect(await selectedIds(page)).toEqual(["block2"]);
    await title(page, "block8").click({modifiers: ["Control"]});
    await title(page, "block7").click({modifiers: ["Shift"]});
    expect(await selectedIds(page)).toEqual(["block2", "block7", "block8"]);
    // Ctrl-click on a selected block unselects it; a plain click selects it alone.
    await title(page, "block8").click({modifiers: ["Control"]});
    expect(await selectedIds(page)).toEqual(["block2", "block7"]);
    await title(page, "block7").click();
    expect(await selectedIds(page)).toEqual(["block7"]);

    await page.keyboard.press("Escape");
    expect(await selectedIds(page)).toEqual([]);

    await page.keyboard.press("Control+a");
    expect(await selectedIds(page)).toHaveLength(8);
    const canvas = await emptyCanvas(page);
    await page.mouse.click(canvas.x, canvas.y);
    expect(await selectedIds(page)).toEqual([]);
});

test("a Shift-drag on the canvas selects the blocks inside the marquee", async ({page, engine}) => {
    await openExample(page, engine.url);
    const before = await blockPosition(page, "block1");
    // From the empty top-left corner to between block3 and block7 and above block8's middle:
    // the middles of block1, block2 and block3 are inside.
    const start = await emptyCanvas(page);
    const c2 = await blockCentre(page, "block2");
    const c3 = await blockCentre(page, "block3");
    const c7 = await blockCentre(page, "block7");
    const c8 = await blockCentre(page, "block8");
    await page.keyboard.down("Shift");
    await page.mouse.move(start.x, start.y);
    await page.mouse.down();
    await page.mouse.move((c3.x + c7.x) / 2, (c2.y + c8.y) / 2, {steps: 8});
    await page.mouse.up();
    await page.keyboard.up("Shift");
    expect(await selectedIds(page)).toEqual(["block1", "block2", "block3"]);
    // The canvas was not panned.
    expect(await blockPosition(page, "block1")).toEqual(before);
});

test("dragging a selection moves every selected block through the engine", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    await other.connectGraph(G);
    const p7 = await blockPosition(page, "block7");
    const p8 = await blockPosition(page, "block8");
    const p1 = await blockPosition(page, "block1");

    await title(page, "block8").click();
    await title(page, "block7").click({modifiers: ["Control"]});
    const box = (await title(page, "block8").boundingBox())!;
    await page.mouse.move(box.x + 10, box.y + 5);
    await page.mouse.down();
    await page.mouse.move(box.x + 70, box.y + 45, {steps: 10});
    await page.mouse.up();

    // Both blocks were sent; the last commands are the final positions.
    const moved = new Set<string>();
    while (moved.size < 2)
        moved.add((await other.next(m => m.command === "changenode")).payload.id);
    expect([...moved].sort()).toEqual(["block7", "block8"]);

    await expect.poll(async () => (await blockPosition(page, "block8")).x).toBeGreaterThan(p8.x);
    await expect.poll(async () => {
        const n7 = await blockPosition(page, "block7");
        const n8 = await blockPosition(page, "block8");
        return Math.abs((n7.x - p7.x) - (n8.x - p8.x)) + Math.abs((n7.y - p7.y) - (n8.y - p8.y));
    }).toBeLessThan(0.01);
    expect(await blockPosition(page, "block1")).toEqual(p1);
    expect(await selectedIds(page)).toEqual(["block7", "block8"]);
});

test("Delete removes the selected blocks through the engine", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    await other.connectGraph(G);

    await title(page, "block1").click();
    await title(page, "block8").click({modifiers: ["Shift"]});
    await page.keyboard.press("Delete");
    const removed = [(await other.next(m => m.command === "removenode")).payload.id,
        (await other.next(m => m.command === "removenode")).payload.id];
    expect(removed.sort()).toEqual(["block1", "block8"]);
    await expect(blocks(page)).toHaveCount(6);
    expect(await selectedIds(page)).toEqual([]);

    // With nothing selected, Delete and Backspace do nothing.
    await page.keyboard.press("Backspace");
    await page.keyboard.press("Delete");
    // graph:list is answered by network:status (as in connectGraph): no removenode before it.
    other.send("graph", "list", {});
    const next = await other.next(m => m.command === "removenode" || m.command === "status");
    expect(next.command).toBe("status");
    await expect(blocks(page)).toHaveCount(6);
});

test("the selection follows a block renamed by another client", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    await other.connectGraph(G);

    await title(page, "block1").click();
    await other.command("graph", "renamenode", {graph: G, from: "block1", to: "first"});
    await expect(block(page, "first")).toHaveCount(1);
    expect(await selectedIds(page)).toEqual(["first"]);
    await page.keyboard.press("Backspace");
    expect((await other.next(m => m.command === "removenode")).payload.id).toBe("first");
    await expect(blocks(page)).toHaveCount(7);
});
