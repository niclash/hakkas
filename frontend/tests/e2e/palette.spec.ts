import type {Page} from "@playwright/test";
import {expect, test} from "./fixtures";
import {EXAMPLE_GRAPH} from "./paths";
import {block, blocks, loggedIn, openProcess} from "./ui";

// 06 §4.5: the palette's search, its tooltips, adding a block by a click or by dragging it, and
// the sockets colored by port type.

const G = "0826152e-3014-4c89-9380-ec4991a76af2";

async function openExample(page: Page, url: string) {
    await loggedIn(page, url);
    await openProcess(page, "Simple Example");
    await expect(blocks(page)).toHaveCount(8);
}

const entry = (page: Page, type: string) => page.locator(`[data-testid=block-type][data-type="${type}"]`);

test("the search keeps the matching types, Escape shows them all again", async ({page, engine}) => {
    await openExample(page, engine.url);
    const all = await page.getByTestId("block-type").count();
    const search = page.getByTestId("palette-search");
    await search.fill("greater");
    await expect(page.getByTestId("block-type")).toHaveText(["Greater", "GreaterEqual"]);
    await search.fill("zzz");
    await expect(page.getByTestId("palette-none")).toBeVisible();
    await search.press("Escape");
    await expect(page.getByTestId("block-type")).toHaveCount(all);

    // A family closed by hand opens for a search that matches in it.
    await page.locator("details[data-family=math] > summary").click();
    await expect(entry(page, "math/Add2")).toBeHidden();
    await search.fill("add");
    await expect(entry(page, "math/Add2")).toBeVisible();
});

test("a type's tooltip has its description and ports", async ({page, engine}) => {
    await openExample(page, engine.url);
    const tooltip = await entry(page, "math/Add2").getAttribute("title");
    expect(tooltip).toContain("math/Add2");
    expect(tooltip).toContain("Inputs: in1 (number), in2 (number)");
    expect(tooltip).toContain("Outputs: out (number)");
});

test("a click adds the block in the middle of the view", async ({page, engine}) => {
    await openExample(page, engine.url);
    await entry(page, "math/Add2").click();
    await expect(block(page, "Add2_1")).toHaveCount(1);
    const editor = (await page.getByTestId("editor").boundingBox())!;
    const box = (await block(page, "Add2_1").boundingBox())!;
    const cx = editor.x + editor.width / 2, cy = editor.y + editor.height / 2;
    expect(box.x < cx && cx < box.x + box.width && box.y < cy && cy < box.y + box.height).toBe(true);
});

test("a type dragged onto the editor is added where it is dropped", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    await other.connectGraph(G);
    const editor = (await page.getByTestId("editor").boundingBox())!;
    // Below block5, the bottom-left block (in the editor element's own coordinates).
    const b5 = (await block(page, "block5").boundingBox())!;
    const at = {x: b5.x - editor.x + 5, y: Math.min(b5.y + b5.height + 20 - editor.y, editor.height - 80)};
    await entry(page, "logic/Not").dragTo(page.getByTestId("editor"), {targetPosition: at});
    const added = await other.next(m => m.command === "addnode");
    expect(added.payload).toMatchObject({id: "Not_1", component: "logic/Not"});
    const box = (await block(page, "Not_1").boundingBox())!;
    // Within a few pixels: a drag's drop coordinates are not exact.
    expect(Math.abs(box.x - (editor.x + at.x))).toBeLessThan(4);
    expect(Math.abs(box.y - (editor.y + at.y))).toBeLessThan(4);
});

test.describe("with bool and string ports", () => {
    test.use({
        graphs: [{
            id: "types", name: "Types", connections: [],
            blocks: {
                not: {component: "logic/Not", metadata: {x: 100, y: 100}},
                range: {component: "timing/MinuteRange", metadata: {x: 400, y: 100}},
                add: {component: "math/Add2", metadata: {x: 100, y: 400}},
            },
        }, EXAMPLE_GRAPH],
    });

    test("sockets are colored by their port's type, with a legend", async ({page, engine}) => {
        await loggedIn(page, engine.url);
        await openProcess(page, "Types");
        const socket = (id: string, side: "input" | "output", port: string) =>
            block(page, id).getByTestId(side + "-" + port).getByTestId("socket");
        await expect(socket("not", "input", "in")).toHaveAttribute("data-type", "bool");
        await expect(socket("not", "input", "in")).toHaveAttribute("title", "bool");
        await expect(socket("range", "input", "range")).toHaveAttribute("data-type", "string");
        await expect(socket("range", "output", "out")).toHaveAttribute("data-type", "bool");
        await expect(socket("add", "input", "in1")).toHaveAttribute("data-type", "number");
        const color = (l: ReturnType<typeof socket>) => l.evaluate(e => getComputedStyle(e).backgroundColor);
        const colors = new Set([await color(socket("not", "input", "in")), await color(socket("range", "input", "range")),
            await color(socket("add", "input", "in1"))]);
        expect(colors.size).toBe(3);
        await expect(page.getByTestId("socket-legend").locator("li")).toHaveText(["number", "bool", "string"]);
    });
});
