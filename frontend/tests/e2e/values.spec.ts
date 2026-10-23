import type {Locator, Page} from "@playwright/test";
import {expect, test} from "./fixtures";
import {EXAMPLE_GRAPH} from "./paths";
import {block, blocks, inputLabels, loggedIn, openProcess, selectedIds, valueNode} from "./ui";

// 06 §4.3: a fixed value (an initial) is drawn as a value node beside its input, connected to
// it; a line drawn from a free input and dropped on the canvas asks for one.

const G = "0826152e-3014-4c89-9380-ec4991a76af2";

async function openExample(page: Page, url: string) {
    await loggedIn(page, url);
    await openProcess(page, "Simple Example");
    await expect(blocks(page)).toHaveCount(8);
}

const socket = (page: Page, id: string, port: string) =>
    block(page, id).getByTestId("input-" + port).getByTestId("input-socket");

async function middle(locator: Locator): Promise<{ x: number, y: number }> {
    const box = (await locator.boundingBox())!;
    return {x: box.x + box.width / 2, y: box.y + box.height / 2};
}

// A point on the empty canvas, above and left of block1 (the top-left block).
async function emptyCanvas(page: Page): Promise<{ x: number, y: number }> {
    const box = (await block(page, "block1").boundingBox())!;
    return {x: box.x - 10, y: box.y - 10};
}

async function drag(page: Page, from: { x: number, y: number }, to: { x: number, y: number }) {
    await page.mouse.move(from.x, from.y);
    await page.mouse.down();
    await page.mouse.move(to.x, to.y, {steps: 8});
    await page.mouse.up();
}

// The value node is left of the block, level with its input.
async function besideItsInput(page: Page, id: string, port: string): Promise<boolean> {
    const value = (await valueNode(page, id, port).boundingBox())!;
    const box = (await block(page, id).boundingBox())!;
    const input = await middle(socket(page, id, port));
    return value.x + value.width < box.x && Math.abs(value.y + value.height / 2 - input.y) < 2;
}

test("fixed values are drawn as value nodes beside their inputs", async ({page, engine}) => {
    await openExample(page, engine.url);
    await expect(page.getByTestId("value-node")).toHaveCount(2);
    await expect(page.getByTestId("value-connection")).toHaveCount(2);
    await expect(valueNode(page, "block8", "in1").getByTestId("value-text")).toHaveText("12");
    await expect(valueNode(page, "block8", "in2").getByTestId("value-text")).toHaveText("23");
    expect(await besideItsInput(page, "block8", "in1")).toBe(true);
    expect(await besideItsInput(page, "block8", "in2")).toBe(true);
    // The block's own labels are the ports' names.
    await expect(block(page, "block8").getByTestId("input-title")).toHaveText(["in1", "in2"]);

    // They follow their block, whoever moves it.
    const other = await engine.client();
    await other.command("graph", "changenode", {graph: G, id: "block8", metadata: {x: 700, y: 600}});
    await expect.poll(() => besideItsInput(page, "block8", "in1")).toBe(true);
    // And are not blocks: not selected, not dragged.
    await page.keyboard.press("Control+a");
    expect(await selectedIds(page)).toHaveLength(8);
    await page.keyboard.press("Escape");
    const before = await valueNode(page, "block8", "in1").boundingBox();
    const at = await middle(valueNode(page, "block8", "in1").getByTestId("value-text"));
    await drag(page, at, {x: at.x + 80, y: at.y + 60});
    expect(await selectedIds(page)).toEqual([]);
    expect(await valueNode(page, "block8", "in1").boundingBox()).toEqual(before);
});

test("a line from a free input dropped on the canvas asks for its value", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    await other.connectGraph(G);

    await drag(page, await middle(socket(page, "block7", "in3")), await emptyCanvas(page));
    const field = page.getByTestId("value-draft-field");
    await expect(field).toBeFocused();
    await field.fill("x");
    await field.press("Enter");
    await expect(page.getByTestId("value-draft-error")).toHaveText("Enter a number");
    await field.fill("4.5");
    await field.press("Enter");
    expect((await other.next(m => m.command === "addinitial")).payload)
        .toMatchObject({tgt: {node: "block7", port: "in3"}, src: {data: "4.5"}});
    await expect(page.getByTestId("value-draft")).toHaveCount(0);
    await expect(valueNode(page, "block7", "in3").getByTestId("value-text")).toHaveText("4.5");
    expect(await besideItsInput(page, "block7", "in3")).toBe(true);

    // Escape, or a click on the canvas, drops the box without sending anything.
    await drag(page, await middle(socket(page, "block7", "in4")), await emptyCanvas(page));
    await page.getByTestId("value-draft-field").press("Escape");
    await expect(page.getByTestId("value-draft")).toHaveCount(0);
    await drag(page, await middle(socket(page, "block7", "in4")), await emptyCanvas(page));
    const canvas = await emptyCanvas(page);
    await page.mouse.click(canvas.x, canvas.y + 5);
    await expect(page.getByTestId("value-draft")).toHaveCount(0);
    expect(await inputLabels(page, "block7")).toEqual(["in1", "in2", "in3 = 4.5", "in4"]);
});

test("double-clicking a value node changes the value", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    await other.connectGraph(G);

    await valueNode(page, "block8", "in2").dblclick();
    const field = valueNode(page, "block8", "in2").getByTestId("value-node-field");
    await expect(field).toBeFocused();
    await field.fill("42");
    await field.press("Enter");
    expect((await other.next(m => m.command === "addinitial")).payload)
        .toMatchObject({tgt: {node: "block8", port: "in2"}, src: {data: "42"}});
    await expect(valueNode(page, "block8", "in2").getByTestId("value-text")).toHaveText("42");

    // Escape leaves it as it was.
    await valueNode(page, "block8", "in2").dblclick();
    await valueNode(page, "block8", "in2").getByTestId("value-node-field").fill("1");
    await valueNode(page, "block8", "in2").getByTestId("value-node-field").press("Escape");
    await expect(valueNode(page, "block8", "in2").getByTestId("value-text")).toHaveText("42");
});

test("pulling a value's line off its input clears it, dropping it on another input moves it", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    await other.connectGraph(G);

    await drag(page, await middle(socket(page, "block8", "in2")), await emptyCanvas(page));
    expect((await other.next(m => m.command === "removeinitial")).payload).toMatchObject({tgt: {node: "block8", port: "in2"}});
    await expect(valueNode(page, "block8", "in2")).toHaveCount(0);
    await expect(page.getByTestId("value-draft")).toHaveCount(0);

    await drag(page, await middle(socket(page, "block8", "in1")), await middle(socket(page, "block7", "in4")));
    expect((await other.next(m => m.command === "removeinitial")).payload).toMatchObject({tgt: {node: "block8", port: "in1"}});
    expect((await other.next(m => m.command === "addinitial")).payload)
        .toMatchObject({tgt: {node: "block7", port: "in4"}, src: {data: "12"}});
    await expect(valueNode(page, "block7", "in4").getByTestId("value-text")).toHaveText("12");
    await expect(page.getByTestId("value-node")).toHaveCount(1);
});

test("removing a block removes its value nodes", async ({page, engine}) => {
    await openExample(page, engine.url);
    await block(page, "block8").getByTestId("title").click();
    await page.keyboard.press("Delete");
    await expect(blocks(page)).toHaveCount(7);
    await expect(page.getByTestId("value-node")).toHaveCount(0);
    await expect(page.getByTestId("value-connection")).toHaveCount(0);
});

test.describe("with a bool input", () => {
    test.use({
        graphs: [{
            id: "types", name: "Types", connections: [],
            blocks: {not: {component: "logic/Not", metadata: {x: 300, y: 100}}},
        }, EXAMPLE_GRAPH],
    });

    test("true or false is chosen, and a double-click toggles it", async ({page, engine}) => {
        await loggedIn(page, engine.url);
        await openProcess(page, "Types");
        const box = (await block(page, "not").boundingBox())!;
        await drag(page, await middle(socket(page, "not", "in")), {x: box.x - 120, y: box.y + box.height + 60});
        await page.getByTestId("value-draft-true").click();
        await expect(valueNode(page, "not", "in").getByTestId("value-text")).toHaveText("true");
        await valueNode(page, "not", "in").dblclick();
        await expect(valueNode(page, "not", "in").getByTestId("value-text")).toHaveText("false");
    });
});
