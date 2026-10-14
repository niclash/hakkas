import {Page} from "@playwright/test";
import {chmodSync, readFileSync} from "node:fs";
import {join} from "node:path";
import {expect, test} from "./fixtures";
import {EXAMPLE_GRAPH} from "./paths";
import {blocks, loggedIn, openProcess, processItem} from "./ui";

// 06 §4.6: managing processes from the list. Every action is a command; the list changes with
// the engine's events.

const G = "0826152e-3014-4c89-9380-ec4991a76af2";

const second = {
    id: "second", name: "Second", description: "The other one", connections: [],
    blocks: {lin: {component: "process/Linear", metadata: {x: 100, y: 100}}},
};
// The object first: [path, object] would read as Playwright's [value, options] form.
test.use({graphs: [second, EXAMPLE_GRAPH]});

const row = (page: Page, name: string) => processItem(page, name);

test("start and stop a process that is not open; the open one shows its uptime and description", async ({page, engine}) => {
    await loggedIn(page, engine.url);
    const other = await engine.client();
    await row(page, "Second").hover();
    await row(page, "Second").getByTestId("process-run").click();
    await expect(row(page, "Second").getByTestId("process-state")).toHaveClass(/running/);
    // A start makes it unsaved (D2: saving stores whether it starts at boot).
    await expect(row(page, "Second").getByTestId("process-unsaved")).toBeVisible();
    await openProcess(page, "Second");
    await expect(row(page, "Second").getByTestId("process-uptime")).toHaveText("Running for less than a minute");
    await expect(row(page, "Second").getByTestId("process-description")).toHaveText("The other one");
    await row(page, "Second").getByTestId("process-run").click();
    await expect(row(page, "Second").getByTestId("process-state")).not.toHaveClass(/running/);
    await expect(row(page, "Second").getByTestId("process-uptime")).toHaveText("Stopped");
    await expect(row(page, "Second").getByTestId("process-unsaved")).toHaveCount(0);
    expect(other).toBeTruthy();
});

test("a change makes a process unsaved; Save and Ctrl+S save it with a short notice", async ({page, engine}) => {
    await loggedIn(page, engine.url);
    await openProcess(page, "Simple Example");
    await expect(row(page, "Simple Example").getByTestId("process-save")).toBeDisabled();
    const other = await engine.client();
    await other.command("graph", "addnode", {graph: G, id: "extra", component: "math/Add2"});
    await expect(row(page, "Simple Example").getByTestId("process-unsaved")).toBeVisible();

    await row(page, "Simple Example").getByTestId("process-save").click();
    await expect(page.getByTestId("notice")).toHaveText("Saved “Simple Example”");
    await expect(row(page, "Simple Example").getByTestId("process-unsaved")).toHaveCount(0);
    const file = JSON.parse(readFileSync(join(engine.dataDir, "graphs", G + ".json"), "utf8"));
    expect(Object.keys(file.blocks)).toContain("extra");

    await other.command("graph", "removenode", {graph: G, id: "extra"});
    await expect(row(page, "Simple Example").getByTestId("process-unsaved")).toBeVisible();
    await page.keyboard.press("Control+s");
    await expect(row(page, "Simple Example").getByTestId("process-unsaved")).toHaveCount(0);
    await expect(page.getByTestId("notice")).toBeVisible();
    await expect(page.getByTestId("notice")).toHaveCount(0, {timeout: 5000});
});

test.describe("when saving fails", () => {
    test.use({allowedErrors: [/./]});

    test("the error is shown in a dialog until it is dismissed", async ({page, engine}) => {
        await loggedIn(page, engine.url);
        await openProcess(page, "Simple Example");
        const other = await engine.client();
        await other.command("graph", "addnode", {graph: G, id: "extra", component: "math/Add2"});
        const graphs = join(engine.dataDir, "graphs");
        chmodSync(graphs, 0o555);
        try {
            await row(page, "Simple Example").getByTestId("process-save").click();
            await expect(page.getByTestId("save-error")).toHaveText(`Unable to save graph: Simple Example (${G})`);
            await expect(page.getByTestId("error")).toHaveCount(0);
            await expect(row(page, "Simple Example").getByTestId("process-unsaved")).toBeVisible();
            await page.getByTestId("dialog-ok").click();
            await expect(page.getByTestId("save-error")).toHaveCount(0);
        } finally {
            chmodSync(graphs, 0o755);
        }
    });
});

test("rename a process in place", async ({page, engine}) => {
    await loggedIn(page, engine.url);
    await row(page, "Second").getByTestId("process-name").dblclick();
    const field = page.getByTestId("process-name-field");
    await expect(field).toBeFocused();
    await field.fill("Pumps");
    await field.press("Enter");
    await expect(row(page, "Pumps")).toHaveCount(1);
    await expect(row(page, "Second")).toHaveCount(0);

    // Escape keeps the name; the pen button renames too.
    await row(page, "Pumps").hover();
    await row(page, "Pumps").getByTestId("process-rename").click();
    await page.getByTestId("process-name-field").fill("Nothing");
    await page.getByTestId("process-name-field").press("Escape");
    await expect(row(page, "Pumps")).toHaveCount(1);
    // The open process too.
    await openProcess(page, "Simple Example");
    await row(page, "Simple Example").getByTestId("process-rename").click();
    await page.getByTestId("process-name-field").fill("Example");
    await page.getByTestId("process-name-field").press("Enter");
    await expect(row(page, "Example")).toHaveClass(/active/);
});

test("delete a process after confirming; the open one leaves the editor", async ({page, engine}) => {
    await loggedIn(page, engine.url);
    await openProcess(page, "Simple Example");
    await expect(blocks(page)).toHaveCount(8);
    await row(page, "Simple Example").getByTestId("process-delete").click();
    await expect(page.getByTestId("delete-question")).toContainText("Delete “Simple Example”?");
    await page.getByTestId("dialog-cancel").click();
    await expect(page.getByTestId("process")).toHaveCount(2);

    await row(page, "Simple Example").getByTestId("process-delete").click();
    await page.getByTestId("dialog-ok").click();
    await expect(page.getByTestId("process")).toHaveCount(1);
    await expect(blocks(page)).toHaveCount(0);
    expect(() => readFileSync(join(engine.dataDir, "graphs", G + ".json"))).toThrow();
});

test("a new process opens when it is created", async ({page, engine}) => {
    await loggedIn(page, engine.url);
    await openProcess(page, "Simple Example");
    await page.getByTestId("process-new").click();
    await page.fill("#newProcessName", "Lights");
    await page.fill("#newProcessDescription", "Garden lights");
    await page.getByTestId("dialog-ok").click();
    await expect(row(page, "Lights")).toHaveClass(/active/);
    await expect(row(page, "Lights").getByTestId("process-unsaved")).toBeVisible();
    await expect(row(page, "Lights").getByTestId("process-description")).toHaveText("Garden lights");
    await expect(blocks(page)).toHaveCount(0);
    expect(await row(page, "Lights").getAttribute("data-id"))
        .toMatch(/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
});
