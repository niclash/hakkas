import {expect, test} from "./fixtures";
import {EXAMPLE_GRAPH} from "./paths";
import {blockIds, blocks, loggedIn, openProcess, processItem} from "./ui";

// 06 §2.1: the open process or type in the address.

const G = "0826152e-3014-4c89-9380-ec4991a76af2";

test.use({graphs: [{id: "sec.ond", name: "Second", connections: [],
    blocks: {lin: {component: "process/Linear", metadata: {x: 100, y: 100}}}}, EXAMPLE_GRAPH]});

test("the address follows what is open; back and forward reopen it", async ({page, engine}) => {
    await loggedIn(page, engine.url);
    await openProcess(page, "Simple Example");
    await expect(page).toHaveURL(new RegExp(`/\\?process=${G}$`));
    await expect(page).toHaveTitle("Simple Example — Hakkas");
    await openProcess(page, "Second");
    await expect(page).toHaveURL(/\/\?process=sec\.ond$/);

    await page.goBack();
    await expect(processItem(page, "Simple Example")).toHaveClass(/active/);
    await expect(blocks(page)).toHaveCount(8);
    await page.goForward();
    await expect(processItem(page, "Second")).toHaveClass(/active/);
    expect(await blockIds(page)).toEqual(["lin"]);
});

test("a bookmark opens its process after the login, and a reload keeps it", async ({page, engine}) => {
    await page.goto(engine.url + "?process=sec.ond");
    await page.fill("#user", "alice");
    await page.fill("#password", "alice-password");
    await page.click("button[type=submit]");
    await expect(processItem(page, "Second")).toHaveClass(/active/);
    await expect(blocks(page)).toHaveCount(1);

    await page.reload();
    await expect(processItem(page, "Second")).toHaveClass(/active/);
    await expect(blocks(page)).toHaveCount(1);
});

test("a type's address", async ({page, engine}) => {
    const other = await engine.client();
    other.send("graph", "clear", {id: "user/Pair", name: "Pair", main: false});
    await other.next(m => m.command === "clear");
    await other.command("graph", "addnode", {graph: "user/Pair", id: "p", component: "math/Add2"});
    other.send("network", "persist", {graph: "user/Pair"});
    await loggedIn(page, engine.url);
    await page.locator("[data-testid=open-type][data-type='user/Pair']").click();
    await expect(page).toHaveURL(/\/\?type=user%2FPair$/);
    await expect(page).toHaveTitle("Pair — Hakkas");
    await page.reload();
    await expect(page.getByTestId("type-banner-name")).toHaveText("user/Pair");
});

test("an address naming no process goes back to the start", async ({page, engine}) => {
    await page.goto(engine.url + "?process=nowhere");
    await page.fill("#user", "alice");
    await page.fill("#password", "alice-password");
    await page.click("button[type=submit]");
    await expect(page.getByTestId("error")).toContainText("Unknown graph: nowhere");
    await expect(page).toHaveURL(engine.url);
    await openProcess(page, "Simple Example");
});
