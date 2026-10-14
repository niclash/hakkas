import {Page} from "@playwright/test";
import {expect, OPERATOR, test, VIEWER} from "./fixtures";
import {block, blockPosition, blocks, connections, loggedIn, openProcess, processItem} from "./ui";

// 06 §4.11: the editor offers only what the role may do (02 §2.4); the engine's refusals stay
// the guard.

const G = "0826152e-3014-4c89-9380-ec4991a76af2";

const menu = (page: Page) => page.getByTestId("context-menu");
const actions = (page: Page) => menu(page).getByTestId("menu-item").evaluateAll(e => e.map(i => i.getAttribute("data-action")));
const title = (page: Page, id: string) => block(page, id).getByTestId("title");

test("a viewer gets a read-only editor and can watch values", async ({page, engine}) => {
    await loggedIn(page, engine.url, VIEWER);
    await openProcess(page, "Simple Example");
    await expect(blocks(page)).toHaveCount(8);
    const other = await engine.client();
    await other.connectGraph(G);

    await expect(page.getByTestId("role")).toHaveText("Viewer — read only");
    await expect(page.getByTestId("block-type")).toHaveCount(0);
    const row = processItem(page, "Simple Example");
    for (const action of ["process-run", "process-save", "process-rename", "process-delete"])
        await expect(row.getByTestId(action)).toHaveCount(0);
    await expect(page.getByTestId("process-new")).toHaveCount(0);

    // Menus: only what changes nothing.
    const b1 = (await title(page, "block1").boundingBox())!;
    await page.mouse.click(b1.x - 10, b1.y - 10, {button: "right"});
    expect(await actions(page)).toEqual(["select-all", "zoom-fit", "watch-all"]);
    await page.keyboard.press("Escape");
    await title(page, "block7").click({button: "right"});
    expect(await actions(page)).toEqual(["watch-inputs", "copy"]);
    await page.keyboard.press("Escape");

    // No drag, no delete, no paste or duplicate, no line drawn.
    const before = await blockPosition(page, "block7");
    const box = (await title(page, "block7").boundingBox())!;
    await page.mouse.move(box.x + 10, box.y + 5);
    await page.mouse.down();
    await page.mouse.move(box.x + 80, box.y + 60, {steps: 6});
    await page.mouse.up();
    await title(page, "block7").click();
    await page.keyboard.press("Delete");
    await page.keyboard.press("Control+d");
    const from = block(page, "block8").getByTestId("output-out").getByTestId("output-socket");
    const to = block(page, "block7").getByTestId("input-in3").getByTestId("input-socket");
    await from.hover();
    await page.mouse.down();
    await to.hover();
    await page.mouse.up();
    other.send("graph", "list", {});   // a barrier: nothing sent before it changed anything
    const next = await other.next(m => m.command !== "error" && (m.protocol === "graph" || m.command === "status"));
    expect(next.command).toBe("status");
    expect(await blockPosition(page, "block7")).toEqual(before);
    await expect(blocks(page)).toHaveCount(8);
    await expect(connections(page)).toHaveCount(6);
    await expect(page.getByTestId("error")).toHaveCount(0);

    // The inspector shows, but does not edit.
    await expect(page.getByTestId("inspector-name")).toHaveAttribute("readonly", "");
    await expect(page.getByTestId("inspector-delete")).toHaveCount(0);
    await title(page, "block8").click();
    await expect(page.getByTestId("input-value-in1").getByTestId("value-field")).toHaveAttribute("readonly", "");
    await expect(page.getByTestId("value-clear")).toHaveCount(0);

    // Live values are a viewer's too.
    await page.getByTestId("inspector").locator("[data-port=in1]").getByTestId("port-watch").click();
    await expect(page.getByTestId("port-live")).toHaveText("Live: 12");
});

test("an operator starts and stops, and edits nothing", async ({page, engine}) => {
    await loggedIn(page, engine.url, OPERATOR);
    await openProcess(page, "Simple Example");
    await expect(page.getByTestId("role")).toHaveText("Operator — starts and stops, no editing");
    await expect(page.getByTestId("block-type")).toHaveCount(0);
    const row = processItem(page, "Simple Example");
    await expect(row.getByTestId("process-save")).toHaveCount(0);
    await expect(row.getByTestId("process-delete")).toHaveCount(0);
    await row.getByTestId("process-run").click();
    await expect(row.getByTestId("process-state")).toHaveClass(/running/);
    await row.getByTestId("process-run").click();
    await expect(row.getByTestId("process-state")).not.toHaveClass(/running/);
    await expect(page.getByTestId("error")).toHaveCount(0);
});

test("an editor gets everything, without a role mark", async ({page, engine}) => {
    await loggedIn(page, engine.url);
    await expect(page.getByTestId("role")).toHaveCount(0);
    await expect(page.getByTestId("process-new")).toBeVisible();
});
