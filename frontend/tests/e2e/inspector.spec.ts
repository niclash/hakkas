import {Page} from "@playwright/test";
import {expect, test} from "./fixtures";
import {EXAMPLE_GRAPH} from "./paths";
import {ProtocolClient} from "./protocol-client";
import {block, blocks, loggedIn, openProcess} from "./ui";

// 06 §4.2: the inspector shows the selected blocks, from their types' component:component.

const G = "0826152e-3014-4c89-9380-ec4991a76af2";

async function openExample(page: Page, url: string) {
    await loggedIn(page, url);
    await openProcess(page, "Simple Example");
    await expect(blocks(page)).toHaveCount(8);
}

const inspector = (page: Page) => page.getByTestId("inspector");

function select(page: Page, id: string, modifiers: ("Control" | "Shift")[] = []) {
    return block(page, id).getByTestId("title").click({modifiers});
}

// The engine's own description of a block type.
async function componentOf(client: ProtocolClient, name: string): Promise<any> {
    client.send("component", "list", {});
    return (await client.next(m => m.command === "component" && m.payload.name === name)).payload;
}

test("one selected block: its id, type and ports as the engine describes them", async ({page, engine}) => {
    await openExample(page, engine.url);
    await expect(page.getByTestId("inspector-empty")).toBeVisible();
    const add2 = await componentOf(await engine.client(), "math/Add2");

    await select(page, "block8");
    await expect(page.getByTestId("inspector-name")).toHaveValue("block8");
    await expect(page.getByTestId("inspector-type")).toHaveText("math/Add2");
    await expect(page.getByTestId("inspector-description")).toHaveText(add2.description);
    const inputs = page.getByTestId("inspector-inputs").getByTestId("inspector-port");
    await expect(inputs.getByTestId("port-id")).toHaveText(add2.inPorts.map((p: any) => p.id));
    await expect(inputs.getByTestId("port-type")).toHaveText(add2.inPorts.map((p: any) => p.type));
    const outputs = page.getByTestId("inspector-outputs").getByTestId("inspector-port");
    await expect(outputs.getByTestId("port-id")).toHaveText(add2.outPorts.map((p: any) => p.id));
    for (const [i, p] of add2.inPorts.entries()) {
        if (p.description)
            await expect(inputs.nth(i).getByTestId("port-description")).toHaveText(p.description);
    }

    await page.keyboard.press("Escape");
    await expect(page.getByTestId("inspector-empty")).toBeVisible();
});

test("several selected blocks: their count, and Delete removes them through the engine", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    await other.connectGraph(G);

    await select(page, "block2");
    await select(page, "block1", ["Shift"]);
    await expect(page.getByTestId("inspector-count")).toHaveText("2 blocks selected");
    await expect(page.getByTestId("inspector-ids")).toHaveText("block1, block2");

    await inspector(page).getByTestId("inspector-delete").click();
    const removed = [(await other.next(m => m.command === "removenode")).payload.id,
        (await other.next(m => m.command === "removenode")).payload.id];
    expect(removed.sort()).toEqual(["block1", "block2"]);
    await expect(blocks(page)).toHaveCount(6);
    await expect(page.getByTestId("inspector-empty")).toBeVisible();
});

test("the inspector follows another client's rename and removal", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();

    await select(page, "block8");
    await other.command("graph", "renamenode", {graph: G, from: "block8", to: "sum"});
    await expect(page.getByTestId("inspector-name")).toHaveValue("sum");
    await other.command("graph", "removenode", {graph: G, id: "sum"});
    await expect(page.getByTestId("inspector-empty")).toBeVisible();
});

test.describe("with a block of an unknown type", () => {
    test.use({
        graphs: [{
            id: "second", name: "Second", connections: [],
            blocks: {ghost: {component: "nowhere/Missing", metadata: {x: 400, y: 100}}},
        }, EXAMPLE_GRAPH],
    });

    test("says that its type is not registered", async ({page, engine}) => {
        await loggedIn(page, engine.url);
        await openProcess(page, "Second");
        await select(page, "ghost");
        await expect(page.getByTestId("inspector-type")).toHaveText("nowhere/Missing");
        await expect(page.getByTestId("inspector-unknown")).toBeVisible();
        await expect(page.getByTestId("inspector-port")).toHaveCount(0);
    });
});
