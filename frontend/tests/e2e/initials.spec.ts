import type {Page} from "@playwright/test";
import {expect, test} from "./fixtures";
import {EXAMPLE_GRAPH} from "./paths";
import {block, blocks, inputLabels, loggedIn, openProcess} from "./ui";

// 06 §4.2: fixed values on inputs (initials, 02 §3.4) in the inspector. The value changes with
// the engine's graph:addinitial/removeinitial, which carry it as a string (the command sends a
// number or a bool); a refusal is shown at the input.

const G = "0826152e-3014-4c89-9380-ec4991a76af2";

async function openExample(page: Page, url: string) {
    await loggedIn(page, url);
    await openProcess(page, "Simple Example");
    await expect(blocks(page)).toHaveCount(8);
}

const select = (page: Page, id: string) => block(page, id).getByTestId("title").click();
const input = (page: Page, port: string) => page.getByTestId("inspector").getByTestId("input-value-" + port);

test("each input shows its connection, its fixed value or its default", async ({page, engine}) => {
    await openExample(page, engine.url);
    await select(page, "block7");
    await expect(input(page, "in1").getByTestId("input-state")).toHaveText("Connected from block3.out");
    await expect(input(page, "in1").getByTestId("value-field")).toHaveCount(0);
    await expect(input(page, "in3").getByTestId("input-state")).toHaveText("Default: 0");
    await expect(input(page, "in3").getByTestId("value-field")).toHaveValue("");

    await select(page, "block8");
    await expect(input(page, "in1").getByTestId("input-state")).toHaveText("Fixed value");
    await expect(input(page, "in1").getByTestId("value-field")).toHaveValue("12");
    await expect(input(page, "in2").getByTestId("value-field")).toHaveValue("23");
});

test("a number typed sets the fixed value through the engine, empty or × clears it", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    await other.connectGraph(G);
    await select(page, "block7");

    const field = input(page, "in3").getByTestId("value-field");
    await field.fill("4.5");
    await field.press("Enter");
    expect((await other.next(m => m.command === "addinitial")).payload)
        .toMatchObject({graph: G, tgt: {node: "block7", port: "in3"}, src: {data: "4.5"}});
    await expect(input(page, "in3").getByTestId("input-state")).toHaveText("Fixed value");
    await expect.poll(() => inputLabels(page, "block7")).toEqual(["in1", "in2", "in3 = 4.5", "in4"]);

    // Text that is not a number is not sent.
    await field.fill("4.2 V");
    await field.press("Enter");
    await expect(input(page, "in3").getByTestId("value-error")).toHaveText("Enter a number");
    await field.press("Escape");
    await expect(field).toHaveValue("4.5");
    await expect(input(page, "in3").getByTestId("value-error")).toHaveCount(0);

    await field.fill("");
    await field.press("Enter");
    expect((await other.next(m => m.command === "removeinitial")).payload).toMatchObject({tgt: {node: "block7", port: "in3"}});
    await expect(input(page, "in3").getByTestId("input-state")).toHaveText(/^Default/);
    await expect.poll(() => inputLabels(page, "block7")).toEqual(["in1", "in2", "in3", "in4"]);

    await select(page, "block8");
    await input(page, "in2").getByTestId("value-clear").click();
    expect((await other.next(m => m.command === "removeinitial")).payload).toMatchObject({tgt: {node: "block8", port: "in2"}});
    await expect.poll(() => inputLabels(page, "block8")).toEqual(["in1 = 12", "in2"]);
    await expect(input(page, "in2").getByTestId("value-clear")).toHaveCount(0);
});

test("a value being typed is sent when another block is clicked", async ({page, engine}) => {
    await openExample(page, engine.url);
    await select(page, "block8");
    await input(page, "in1").getByTestId("value-field").fill("7");
    await select(page, "block7");
    await expect.poll(() => inputLabels(page, "block8")).toEqual(["in1 = 7", "in2 = 23"]);
});

test("a refused value is shown at the input, not with the other errors", async ({page, engine}) => {
    await openExample(page, engine.url);
    const other = await engine.client();
    await select(page, "block7");
    // The page's addinitial is held back while another client connects that input, so the
    // engine refuses it (no fan-in, 02 §3.4.1).
    await page.evaluate(() => {
        const w = window as any;
        const send = WebSocket.prototype.send;
        w.__held = [];
        WebSocket.prototype.send = function (data: any) {
            if (typeof data === "string" && data.includes('"addinitial"'))
                w.__held.push(() => send.call(this, data));
            else
                send.call(this, data);
        };
    });
    const field = input(page, "in3").getByTestId("value-field");
    await field.fill("1");
    await field.press("Enter");
    await other.command("graph", "addedge", {graph: G, src: {node: "block8", port: "out"}, tgt: {node: "block7", port: "in3"}});
    await expect(input(page, "in3").getByTestId("input-state")).toHaveText("Connected from block8.out");
    await page.evaluate(() => (window as any).__held.forEach((f: () => void) => f()));
    await expect(input(page, "in3").getByTestId("value-error")).toHaveText("Input already connected: block7.in3");
    await expect(page.getByTestId("error")).toHaveCount(0);
});

test.describe("with bool and string inputs", () => {
    test.use({
        graphs: [{
            id: "types", name: "Types", connections: [],
            blocks: {
                not: {component: "logic/Not", metadata: {x: 100, y: 100}},
                range: {component: "timing/MinuteRange", metadata: {x: 400, y: 100}},
            },
        }, EXAMPLE_GRAPH],
    });

    test("a switch for a bool input, a text field for a string input", async ({page, engine}) => {
        await loggedIn(page, engine.url);
        await openProcess(page, "Types");
        const other = await engine.client();
        await other.connectGraph("types");

        await select(page, "not");
        const toggle = input(page, "in").getByTestId("value-switch");
        await expect(toggle).not.toBeChecked();
        await toggle.check();
        expect((await other.next(m => m.command === "addinitial")).payload).toMatchObject({src: {data: "true"}});
        await expect(input(page, "in").getByTestId("input-state")).toHaveText("Fixed value");
        await expect(toggle).toBeChecked();
        await toggle.uncheck();
        expect((await other.next(m => m.command === "addinitial")).payload).toMatchObject({src: {data: "false"}});
        await expect.poll(() => inputLabels(page, "not")).toEqual(["in = false"]);

        await select(page, "range");
        const field = input(page, "range").getByTestId("value-field");
        await field.fill("08:00-17:00");
        await field.press("Enter");
        expect((await other.next(m => m.command === "addinitial")).payload).toMatchObject({src: {data: "08:00-17:00"}});
        await expect.poll(() => inputLabels(page, "range")).toEqual(["range = 08:00-17:00"]);
    });
});
