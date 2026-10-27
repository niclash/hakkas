import type {Page} from "@playwright/test";
import {readFileSync} from "node:fs";
import {expect, test, VIEWER} from "./fixtures";
import {EXAMPLE_GRAPH} from "./paths";
import {loggedIn, processItem} from "./ui";

// 06 §4.17 (plan task 79): the process map — every process with its ports, the links between
// them (02 §3.8), drawn, pulled and deleted through the engine, undone, followed from another
// client; a process opened from it; its address.

const P = {id: "p", name: "Producer", description: "Makes a value", blocks: {s: {component: "math/Add2", metadata: {x: 0, y: 0}}},
    connections: [], inports: [], outports: [{name: "o", block: "s", port: "out"}]};
const Q = {id: "q", name: "Consumer", blocks: {t: {component: "math/Add2", metadata: {x: 0, y: 0}}},
    connections: [{src: {data: 1}, tgt: {block: "t", port: "in2"}}],
    inports: [{name: "i", block: "t", port: "in1"}, {name: "i2", block: "t", port: "in2"}], outports: []};
const LINK = {src: {graph: "p", port: "o"}, tgt: {graph: "q", port: "i"}};

// A path second: two objects first would be read as Playwright's [value, options].
test.use({graphs: [P, EXAMPLE_GRAPH, Q]});

const map = (page: Page) => page.getByTestId("map");
const node = (page: Page, graph: string) => page.locator(`[data-testid=map-process][data-graph="${graph}"]`);
const socket = (page: Page, graph: string, side: "in" | "out", port: string) =>
    node(page, graph).locator(`[data-testid=map-${side}port][data-port="${port}"]`).getByTestId("map-socket");
const link = (page: Page, name: string) => page.locator(`[data-testid=map-link][data-link="${name}"]`);

async function openMap(page: Page) {
    await page.getByTestId("process-map").click();
    await expect(map(page)).toHaveAttribute("data-settled", "true");
}

async function drag(page: Page, from: ReturnType<typeof socket>, to: ReturnType<typeof socket>) {
    await from.hover();
    await page.mouse.down();
    await to.hover();
    await page.mouse.up();
}

test("shows every process with its ports; a link drawn, followed, undone and deleted", async ({page, engine}) => {
    await loggedIn(page, engine.url);
    await openMap(page);
    await expect(page).toHaveURL(/\/\?map$/);
    await expect(page).toHaveTitle("Process map — Hakkas");
    await expect(page.getByTestId("inspector")).toHaveCount(0);
    await expect(page.getByTestId("map-process")).toHaveCount(3);
    await expect(node(page, "p").getByTestId("map-process-name")).toHaveText("Producer");
    await expect(node(page, "p")).toHaveAttribute("title", "Makes a value");
    await expect(node(page, "q").getByTestId("map-inport")).toHaveText(["i", "i2"]);
    await expect(node(page, "p").getByTestId("map-outport")).toHaveText(["o"]);

    const other = await engine.client();
    await drag(page, socket(page, "p", "out", "o"), socket(page, "q", "in", "i"));
    expect((await other.next(m => m.protocol === "link" && m.command === "add")).payload).toEqual(LINK);
    await expect(link(page, "p.o>q.i")).toHaveCount(1);

    // Undone and redone (06 §4.13): the map's own steps.
    await page.keyboard.press("Control+z");
    expect((await other.next(m => m.protocol === "link" && m.command === "remove")).payload).toEqual(LINK);
    await expect(link(page, "p.o>q.i")).toHaveCount(0);
    await page.keyboard.press("Control+Shift+z");
    await other.next(m => m.protocol === "link" && m.command === "add");
    await expect(link(page, "p.o>q.i")).toHaveCount(1);

    // An inport with a value is refused, with the engine's message.
    await drag(page, socket(page, "p", "out", "o"), socket(page, "q", "in", "i2"));
    await expect(page.getByTestId("error")).toContainText("Input already connected: q.i2");
    await expect(page.locator("[data-testid=map-link]")).toHaveCount(1);

    // Deleted from its menu.
    await link(page, "p.o>q.i").locator("path").click({button: "right"});
    await page.getByTestId("context-menu").locator('[data-action="delete-link"]').click();
    await other.next(m => m.protocol === "link" && m.command === "remove");
    await expect(link(page, "p.o>q.i")).toHaveCount(0);

    // Another client's link, its port renamed (the link ends), a new process.
    await other.command("link", "add", LINK);
    await expect(link(page, "p.o>q.i")).toHaveCount(1);
    await other.connectGraph("q");
    await other.command("graph", "renameinport", {graph: "q", from: "i", to: "input"});
    await expect(link(page, "p.o>q.i")).toHaveCount(0);
    await expect(node(page, "q").getByTestId("map-inport")).toHaveText(["input", "i2"]);
    await other.command("runtime", "new_graph", {graph: "r", name: "Third"});
    await expect(page.getByTestId("map-process")).toHaveCount(4);
});

test("a double-click opens a process; back returns to the map", async ({page, engine}) => {
    await loggedIn(page, engine.url);
    await openMap(page);
    await node(page, "q").getByTestId("map-process-name").dblclick();
    await expect(processItem(page, "Consumer")).toHaveClass(/active/);
    await expect(page.getByTestId("editor")).toBeVisible();
    await expect(map(page)).toHaveCount(0);
    await expect(page).toHaveURL(/process=q/);
    await page.goBack();
    await expect(map(page)).toHaveAttribute("data-settled", "true");
    await page.goForward();
    await expect(page.getByTestId("editor")).toBeVisible();
    await expect(processItem(page, "Consumer")).toHaveClass(/active/);

    // A process's menu opens it too.
    await openMap(page);
    await node(page, "p").getByTestId("map-process-name").click({button: "right"});
    await page.getByTestId("context-menu").locator('[data-action="open-process"]').click();
    await expect(processItem(page, "Producer")).toHaveClass(/active/);
    await expect(map(page)).toHaveCount(0);
});

test.describe("a viewer", () => {
    test("sees the map and its links, and cannot draw one", async ({page, engine}) => {
        const other = await engine.client();
        await other.command("link", "add", LINK);
        await loggedIn(page, engine.url, VIEWER);
        await openMap(page);
        await expect(link(page, "p.o>q.i")).toHaveCount(1);
        await drag(page, socket(page, "p", "out", "o"), socket(page, "q", "in", "i2"));
        await page.waitForTimeout(300);
        await expect(page.locator("[data-testid=map-link]")).toHaveCount(1);
        await link(page, "p.o>q.i").locator("path").click({button: "right"});
        await expect(page.getByTestId("context-menu")).toHaveCount(0);
    });
});

// 06 §4.16: a backup holds the links and a restore makes them; a process deployed again keeps its
// links, which deleting it (to replace it) would end.
test("links in a backup and a restore; a replaced process keeps its links", async ({page, engine}) => {
    const other = await engine.client();
    await other.command("link", "add", LINK);
    await loggedIn(page, engine.url);
    const downloaded = async (click: () => Promise<void>) => {
        const download = page.waitForEvent("download");
        await click();
        return JSON.parse(readFileSync(await (await download).path(), "utf8"));
    };
    const upload = async (file: object) => {
        await page.getByTestId("process-upload-file").setInputFiles({name: "x.json", mimeType: "application/json",
            buffer: Buffer.from(JSON.stringify(file))});
        await expect(page.getByTestId("transfer-upload")).toBeEnabled();
        await expect(page.getByTestId("transfer-links")).toHaveText("1 link between processes made afterwards");
        await page.getByTestId("transfer-upload").click();
        await expect(page.locator('[data-testid=transfer-report][data-id="p.o → q.i"]')).toContainText("done");
        await page.getByTestId("transfer-close").click();
    };
    const links = async () => {
        other.send("link", "list", {});
        return (await other.next(m => m.protocol === "link" && m.command === "links")).payload.links;
    };

    const backup = await downloaded(() => page.getByTestId("process-backup").click());
    expect(backup.links).toEqual([LINK]);
    const consumer = await downloaded(async () => {
        await processItem(page, "Consumer").hover();
        await processItem(page, "Consumer").getByTestId("process-download").click();
    });
    expect(consumer.links).toBeUndefined();

    // Consumer deployed again: replaced, and its link made again.
    await upload(consumer);
    expect(await links()).toEqual([LINK]);

    // Both deleted, then the backup restored.
    for (const [graph, name] of [["p", "Producer"], ["q", "Consumer"]]) {
        other.send("runtime", "delete_graph", {id: graph, name});
        await other.next(m => m.command === "delete_graph" && m.payload.graph === graph);
    }
    expect(await links()).toEqual([]);
    await upload(backup);
    expect(await links()).toEqual([LINK]);
});
