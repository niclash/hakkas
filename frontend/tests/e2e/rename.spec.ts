import {Page} from "@playwright/test";
import {expect, test} from "./fixtures";
import {ProtocolClient} from "./protocol-client";
import {block, blockIds, blocks, loggedIn, openProcess} from "./ui";

// 06 §4.2: renaming a block from the inspector, with F2 or from the block's menu. The block
// changes with the engine's graph:renamenode; a refusal is shown under the name field.

const G = "0826152e-3014-4c89-9380-ec4991a76af2";

async function openExample(page: Page, url: string) {
    await loggedIn(page, url);
    await openProcess(page, "Simple Example");
    await expect(blocks(page)).toHaveCount(8);
}

const title = (page: Page, id: string) => block(page, id).getByTestId("title");
const nameField = (page: Page) => page.getByTestId("inspector-name");

async function renamed(other: ProtocolClient): Promise<{ from: string, to: string }> {
    return (await other.next(m => m.command === "renamenode")).payload;
}

test("a name typed in the inspector renames the block through the engine", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    await other.connectGraph(G);

    await title(page, "block8").click();
    await nameField(page).fill("sum");
    await nameField(page).press("Enter");
    expect(await renamed(other)).toMatchObject({graph: G, from: "block8", to: "sum"});
    await expect(block(page, "sum")).toHaveCount(1);
    await expect(block(page, "block8")).toHaveCount(0);
    await expect(nameField(page)).toHaveValue("sum");

    // Leaving the field sends a changed name too.
    await nameField(page).fill("total");
    await title(page, "block7").click();
    expect(await renamed(other)).toMatchObject({from: "sum", to: "total"});
    await expect(block(page, "total")).toHaveCount(1);
    await expect(nameField(page)).toHaveValue("block7");
});

test("a refused name is shown at the field, and Escape puts the id back", async ({page, engine}) => {
    await openExample(page, engine.url);
    await title(page, "block8").click();

    await nameField(page).fill("block1");
    await nameField(page).press("Enter");
    await expect(page.getByTestId("inspector-name-error")).toHaveText("Node exists: block1");
    await expect(nameField(page)).toHaveAttribute("aria-invalid", "true");
    // Not with the other errors.
    await expect(page.getByTestId("error")).toHaveCount(0);
    expect(await blockIds(page)).toContain("block8");

    await nameField(page).fill("x".repeat(65));
    await nameField(page).press("Enter");
    await expect(page.getByTestId("inspector-name-error")).toHaveText("Invalid id");

    await nameField(page).press("Escape");
    await expect(nameField(page)).toHaveValue("block8");
    await expect(page.getByTestId("inspector-name-error")).toHaveCount(0);
    // Escape in the field leaves the selection alone.
    await expect(nameField(page)).toBeVisible();

    // An unchanged name sends nothing.
    await nameField(page).press("Enter");
    await expect(page.getByTestId("inspector-name-error")).toHaveCount(0);
    await expect(page.getByTestId("error")).toHaveCount(0);
});

test("F2 opens the name field of the selected block", async ({page, engine}) => {
    await openExample(page, engine.url);
    await title(page, "block3").click();
    await page.keyboard.press("F2");
    await expect(nameField(page)).toBeFocused();
    // The whole name is selected, so typing replaces it.
    await page.keyboard.type("mid");
    await page.keyboard.press("Enter");
    await expect(block(page, "mid")).toHaveCount(1);

    // Not with several blocks selected.
    await title(page, "block1").click({modifiers: ["Shift"]});
    await page.keyboard.press("F2");
    await expect(page.getByTestId("inspector-count")).toHaveText("2 blocks selected");
});

test("the block's menu has Rename", async ({page, engine}) => {
    await openExample(page, engine.url);
    await title(page, "block1").click();
    await title(page, "block6").click({button: "right"});
    await page.getByTestId("context-menu").locator("[data-action=rename]").click();
    await expect(nameField(page)).toBeFocused();
    await expect(nameField(page)).toHaveValue("block6");
    await page.keyboard.type("lower");
    await page.keyboard.press("Enter");
    await expect(block(page, "lower")).toHaveCount(1);
});
