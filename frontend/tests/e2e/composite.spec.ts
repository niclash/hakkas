import {Page} from "@playwright/test";
import {expect, test} from "./fixtures";
import {block, blocks, inputLabels, loggedIn, openProcess, processItem} from "./ui";

// 06 §4.9: a composite block type made from selected blocks — the port mapping dialog — built
// as its component graph and persisted (02 §3.4.3); opened, saved and deleted from the editor.

const G = "0826152e-3014-4c89-9380-ec4991a76af2";

async function openExample(page: Page, url: string) {
    await loggedIn(page, url);
    await openProcess(page, "Simple Example");
    await expect(blocks(page)).toHaveCount(8);
}

const item = (page: Page, action: string) => page.getByTestId("context-menu").locator(`[data-action="${action}"]`);
const portRow = (page: Page, ref: string) => page.locator(`[data-testid=composite-port][data-port="${ref}"]`);
const title = (page: Page, id: string) => block(page, id).getByTestId("title");

async function selectAdders(page: Page) {
    await title(page, "block1").click();
    await title(page, "block2").click({modifiers: ["Shift"]});
    await title(page, "block3").click({modifiers: ["Shift"]});
}

test("create a type from three blocks, choosing and naming its ports", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    await selectAdders(page);
    await title(page, "block3").click({button: "right"});
    await expect(item(page, "create-composite")).toHaveText(/from 3 blocks/);
    await item(page, "create-composite").click();

    // block3.out feeds block7, outside: exposed at first; the inputs fed inside are listed too.
    await expect(portRow(page, "block3.out").getByTestId("composite-expose")).toBeChecked();
    await expect(portRow(page, "block3.out").getByTestId("composite-name")).toHaveValue("out");
    await expect(portRow(page, "block1.in1").getByTestId("composite-expose")).not.toBeChecked();
    await expect(page.getByTestId("composite-port")).toHaveCount(4 * 3 + 3);

    await page.getByTestId("composite-type").fill("user/Sum3");
    await page.getByTestId("composite-description").fill("Three adders");
    await portRow(page, "block1.in1").getByTestId("composite-expose").check();
    await portRow(page, "block1.in1").getByTestId("composite-name").fill("a");
    await portRow(page, "block2.in1").getByTestId("composite-expose").check();
    await portRow(page, "block2.in1").getByTestId("composite-name").fill("b");
    await page.getByTestId("dialog-ok").click();

    const made = await other.next(m => m.command === "component" && m.payload.name === "user/Sum3");
    expect(made.payload).toMatchObject({description: "Three adders", subgraph: true});
    expect(made.payload.inPorts.map((p: any) => p.id)).toEqual(["a", "b"]);
    expect(made.payload.outPorts.map((p: any) => p.id)).toEqual(["out"]);
    await expect(page.getByTestId("composite-dialog")).toHaveCount(0);
    await expect(page.getByTestId("notice")).toHaveText("Created block type “user/Sum3”");
    // Save as component: the blocks stay as they were; the type is in the palette.
    await expect(blocks(page)).toHaveCount(8);
    await expect(page.locator("[data-testid=block-type][data-type='user/Sum3']")).toHaveCount(1);
    // It is no process.
    await expect(page.getByTestId("process")).toHaveCount(1);

    // An instance has the chosen ports.
    await page.locator("[data-testid=block-type][data-type='user/Sum3']").click();
    await expect(block(page, "Sum3_1")).toHaveCount(1);
    expect(await inputLabels(page, "Sum3_1")).toEqual(["a", "b"]);
});

test("names are checked before anything is sent", async ({page, engine}) => {
    await openExample(page, engine.url);
    await selectAdders(page);
    await title(page, "block1").click({button: "right"});
    await item(page, "create-composite").click();
    await page.getByTestId("composite-type").fill("math/Add2");
    await portRow(page, "block1.in1").getByTestId("composite-expose").check();
    await portRow(page, "block1.in1").getByTestId("composite-name").fill("out.x");
    await page.getByTestId("dialog-ok").click();
    await expect(page.getByTestId("composite-type-problem")).toHaveText("A block type with this name exists");
    await expect(portRow(page, "block1.in1").getByTestId("composite-name-problem")).toHaveText(/no “.”/);
    await page.getByTestId("dialog-cancel").click();
    await expect(page.getByTestId("composite-dialog")).toHaveCount(0);
});

test("open a type, save it, and delete it once nothing uses it", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    // Made by another client, through the protocol (02 §3.4.3).
    other.send("graph", "clear", {id: "user/Pair", name: "Pair", main: false, description: "", icon: ""});
    await other.next(m => m.command === "clear");
    await other.command("graph", "addnode", {graph: "user/Pair", id: "p", component: "math/Add2", metadata: {x: 0, y: 0}});
    await other.command("graph", "addnode", {graph: "user/Pair", id: "q", component: "math/Add2", metadata: {x: 300, y: 0}});
    await other.command("graph", "addedge", {graph: "user/Pair", src: {node: "p", port: "out"}, tgt: {node: "q", port: "in1"}});
    await other.command("graph", "addinport", {graph: "user/Pair", node: "p", port: "in1", public: "x"});
    await other.command("graph", "addoutport", {graph: "user/Pair", node: "q", port: "out", public: "y"});
    other.send("network", "persist", {graph: "user/Pair"});
    await expect(page.locator("[data-testid=open-type][data-type='user/Pair']")).toHaveCount(1);

    // An instance in the process.
    await page.locator("[data-testid=block-type][data-type='user/Pair']").click();
    await expect(block(page, "Pair_1")).toHaveCount(1);

    // Opened: its blocks, its ports as markers, the banner; no process is open.
    await page.locator("[data-testid=open-type][data-type='user/Pair']").click();
    await expect(page.getByTestId("type-banner-name")).toHaveText("user/Pair");
    await expect(blocks(page)).toHaveCount(2);
    await expect(page.getByTestId("port-node")).toHaveCount(2);
    await expect(processItem(page, "Simple Example")).not.toHaveClass(/active/);

    // In use: neither saved nor deleted.
    await page.getByTestId("type-save").click();
    await expect(page.getByTestId("type-error")).toHaveText("Component in use: user/Pair");
    await page.getByTestId("type-delete").click();
    await page.getByTestId("dialog-ok").click();
    await expect(page.getByTestId("type-error")).toHaveText("Component in use: user/Pair");
    await expect(page.getByTestId("error")).toHaveCount(0);

    // Not in use any more: saved, then deleted.
    await other.command("graph", "removenode", {graph: G, id: "Pair_1"});
    await page.getByTestId("type-save").click();
    await expect(page.getByTestId("notice")).toHaveText("Saved block type “user/Pair”");
    await page.getByTestId("type-delete").click();
    await expect(page.getByTestId("delete-type-question")).toContainText("user/Pair");
    await page.getByTestId("dialog-ok").click();
    await expect(page.getByTestId("type-banner")).toHaveCount(0);
    await expect(blocks(page)).toHaveCount(0);
    await expect(page.locator("[data-testid=block-type][data-type='user/Pair']")).toHaveCount(0);
});
