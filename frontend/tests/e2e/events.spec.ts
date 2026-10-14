import {Page} from "@playwright/test";
import {expect, test, VIEWER} from "./fixtures";
import {block, blockIds, blockPosition, blocks, connections, inputLabels, loggedIn, openProcess} from "./ui";

// 02 §1.1, 06 §3: the view changes only from the engine's events — whoever sent the command.

const G = "0826152e-3014-4c89-9380-ec4991a76af2";

async function openExample(page: Page, url: string) {
    await loggedIn(page, url);
    await openProcess(page, "Simple Example");
    await expect(blocks(page)).toHaveCount(8);
}

test("another client's edits appear", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();

    expect((await other.command("graph", "changenode", {graph: G, id: "block8", metadata: {x: 900, y: 50}})).command).toBe("changenode");
    await expect.poll(() => blockPosition(page, "block8")).toEqual({x: 900, y: 50});

    await other.command("graph", "renamenode", {graph: G, from: "block1", to: "first"});
    await expect(block(page, "first")).toHaveCount(1);
    await expect(block(page, "block1")).toHaveCount(0);

    await other.command("graph", "addinitial", {graph: G, tgt: {node: "block7", port: "in3"}, src: {data: "4.5"}});
    await expect.poll(() => inputLabels(page, "block7")).toEqual(["in1", "in2", "in3 = 4.5", "in4"]);
    await other.command("graph", "removeinitial", {graph: G, tgt: {node: "block8", port: "in1"}});
    await expect.poll(() => inputLabels(page, "block8")).toEqual(["in1", "in2 = 23"]);

    await other.command("graph", "addnode", {graph: G, id: "mult", component: "math/Mult2", metadata: {x: 700, y: 700}});
    await other.command("graph", "addedge", {graph: G, src: {node: "block7", port: "out"}, tgt: {node: "mult", port: "in1"}});
    await expect(block(page, "mult")).toHaveCount(1);
    await expect(connections(page)).toHaveCount(7);

    await other.command("graph", "removeedge", {graph: G, src: {node: "block3", port: "out"}, tgt: {node: "block7", port: "in1"}});
    await expect(connections(page)).toHaveCount(6);

    // Removing a block removes its connections first (02 §1.1, compound effects).
    await other.command("graph", "removenode", {graph: G, id: "block7"});
    await expect(blocks(page)).toHaveCount(8);
    await expect(block(page, "block7")).toHaveCount(0);
    await expect(connections(page)).toHaveCount(4);
});

test("a block type picked in the palette is added by the engine", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    await other.connectGraph(G);

    await page.locator("[data-type='math/Add2']").click();
    const added = await other.next(m => m.command === "addnode");
    expect(added.payload).toMatchObject({graph: G, id: "Add2_1", component: "math/Add2"});
    await expect(block(page, "Add2_1")).toHaveCount(1);
    expect(await inputLabels(page, "Add2_1")).toEqual(["in1", "in2"]);

    await page.locator("[data-type='math/Add2']").click();
    expect((await other.next(m => m.command === "addnode")).payload.id).toBe("Add2_2");
    await expect(blocks(page)).toHaveCount(10);
});

test("dragging a block moves it through the engine", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    await other.connectGraph(G);

    const before = await blockPosition(page, "block8");
    const title = block(page, "block8").getByTestId("title");
    const box = (await title.boundingBox())!;
    await page.mouse.move(box.x + 10, box.y + 5);
    await page.mouse.down();
    await page.mouse.move(box.x + 60, box.y + 45, {steps: 5});
    await page.mouse.up();

    const moved = await other.next(m => m.command === "changenode");
    expect(moved.payload.id).toBe("block8");
    // The last event's position is where the block is drawn.
    await expect.poll(async () => {
        const p = await blockPosition(page, "block8");
        return p.x > before.x && p.y > before.y;
    }).toBe(true);
});

test("a refused command changes nothing and shows the engine's error", async ({page, engine}) => {
    // A viewer may not change a process (02 §2.4); the read-only editor is plan task 38.
    await loggedIn(page, engine.url, VIEWER);
    await openProcess(page, "Simple Example");
    const from = block(page, "block8").getByTestId("output-out").getByTestId("output-socket");
    const to = block(page, "block7").getByTestId("input-in3").getByTestId("input-socket");
    await from.hover();
    await page.mouse.down();
    await to.hover();
    await page.mouse.up();

    await expect(page.getByTestId("error")).toContainText("Not permitted: graph:addedge");
    await expect(connections(page)).toHaveCount(6);
    expect(await inputLabels(page, "block7")).toEqual(["in1", "in2", "in3", "in4"]);
});

test("a connection drawn onto an input with a fixed value replaces the value", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    await other.connectGraph(G);
    const from = block(page, "block1").getByTestId("output-out").getByTestId("output-socket");
    const to = block(page, "block8").getByTestId("input-in1").getByTestId("input-socket");
    await from.hover();
    await page.mouse.down();
    await to.hover();
    await page.mouse.up();

    expect((await other.next(m => m.command === "removeinitial")).payload).toMatchObject({tgt: {node: "block8", port: "in1"}});
    expect((await other.next(m => m.command === "addedge")).payload)
        .toMatchObject({src: {node: "block1", port: "out"}, tgt: {node: "block8", port: "in1"}});
    await expect(connections(page)).toHaveCount(7);
    await expect.poll(() => inputLabels(page, "block8")).toEqual(["in1", "in2 = 23"]);
});

test("a connection drawn in the editor is made by the engine", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    await other.connectGraph(G);

    const from = block(page, "block8").getByTestId("output-out").getByTestId("output-socket");
    const to = block(page, "block7").getByTestId("input-in3").getByTestId("input-socket");
    await from.hover();
    await page.mouse.down();
    await to.hover();
    await page.mouse.up();

    const added = await other.next(m => m.command === "addedge");
    expect(added.payload).toMatchObject({src: {node: "block8", port: "out"}, tgt: {node: "block7", port: "in3"}});
    await expect(connections(page)).toHaveCount(7);
});

test("events of a process that is not open are not drawn", async ({page, engine}) => {
    await loggedIn(page, engine.url);
    const other = await engine.client();
    // The page has opened nothing, so it receives every process's events (02 §4.1).
    await other.command("graph", "addnode", {graph: G, id: "x", component: "math/Add2"});
    await expect(page.getByTestId("process")).toHaveCount(1);
    await expect(blocks(page)).toHaveCount(0);
    await openProcess(page, "Simple Example");
    await expect(blocks(page)).toHaveCount(9);
    expect(await blockIds(page)).toContain("x");
});
