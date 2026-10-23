import type {Page} from "@playwright/test";
import {expect, test} from "./fixtures";
import {block, blocks, loggedIn, openProcess, processItem} from "./ui";

// 06 §2: a lost connection keeps the editor, read-only, and reconnects with back-off, resuming
// the session; the process that was open is opened again, with its watches.

const G = "0826152e-3014-4c89-9380-ec4991a76af2";

// Keeps the page's WebSockets, so that a test can drop them while the engine runs on.
async function trackSockets(page: Page) {
    await page.addInitScript(() => {
        const Native = WebSocket;
        const sockets: WebSocket[] = [];
        (window as any).__sockets = sockets;
        (window as any).WebSocket = class extends Native {
            constructor(url: string | URL, protocols?: string | string[]) {
                super(url, protocols);
                sockets.push(this);
            }
        };
    });
}

async function dropConnection(page: Page) {
    await page.evaluate(() => (window as any).__sockets.forEach((s: WebSocket) => s.close()));
}

test("a dropped connection is resumed, the process reopened and its watches sent again", async ({page, engine}) => {
    await trackSockets(page);
    await loggedIn(page, engine.url);
    await openProcess(page, "Simple Example");
    await expect(blocks(page)).toHaveCount(8);
    await block(page, "block8").getByTestId("title").click();
    await page.getByTestId("inspector").locator("[data-port=in1]").getByTestId("port-watch").click();
    await expect(page.getByTestId("port-live")).toHaveText("Live: 12");

    await dropConnection(page);
    await expect(page.getByTestId("reconnecting")).toBeVisible();
    // Meanwhile the editor stays, read-only, its values stale.
    await expect(blocks(page)).toHaveCount(8);
    await expect(page.getByTestId("block-type")).toHaveCount(0);
    await expect(page.locator("[data-testid=live-value].stale")).toHaveCount(1);

    // Back: no password asked, the same process open, the watch on again.
    await expect(page.getByTestId("reconnecting")).toHaveCount(0, {timeout: 8000});
    await expect(page.getByTestId("logout")).toBeVisible();
    await expect(processItem(page, "Simple Example")).toHaveClass(/active/);
    await expect(blocks(page)).toHaveCount(8);
    await expect(page.locator("[data-testid=live-value][data-target='block8.in1']")).toHaveText("12");
    await expect(page.locator("[data-testid=live-value].stale")).toHaveCount(0);
    await expect(page.getByTestId("block-type").first()).toBeVisible();

    // Editing works again.
    const other = await engine.client();
    await other.connectGraph(G);
    await page.locator("[data-type='math/Add2']").click();
    expect((await other.next(m => m.command === "addnode")).payload.id).toBe("Add2_1");
});

// (The first automatic attempt comes after a second too, so this shows the button works, not
// that it is quicker.)
test("Retry now reconnects", async ({page, engine}) => {
    await trackSockets(page);
    await loggedIn(page, engine.url);
    await dropConnection(page);
    await expect(page.getByTestId("reconnecting")).toBeVisible();
    await page.getByTestId("reconnect-now").click();
    await expect(page.getByTestId("reconnecting")).toHaveCount(0, {timeout: 2000});
});

test("after an engine restart the session has ended: log in again", async ({page, engine}) => {
    await loggedIn(page, engine.url);
    await engine.restart();
    await expect(page.getByTestId("login-message"))
        .toHaveText("The connection was lost and the session has ended: log in again.", {timeout: 15000});
});
