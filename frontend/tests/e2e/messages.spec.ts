import {expect, test} from "./fixtures";
import {blocks, loggedIn, openProcess} from "./ui";

// 06 §4.12: the messages panel (the engine's mirrored log, 05 §4, and the refusals), the user
// in the top bar, and the About view.

test.describe("with the log mirrored", () => {
    test.use({config: {log: {mirror: true, level: "info"}}});

    test("the engine's log lines arrive in the panel, filtered by level; unread ones are counted", async ({page, engine}) => {
        await loggedIn(page, engine.url);
        await expect(page.getByTestId("messages")).toHaveCount(0);
        // Another login is logged, and mirrored to this session.
        await engine.client();
        await expect(page.getByTestId("messages-unread")).toBeVisible();

        await page.getByTestId("messages-toggle").click();
        await expect(page.getByTestId("messages-unread")).toHaveCount(0);
        const line = page.getByTestId("message").filter({hasText: "login: user 'alice'"}).last();
        await expect(line).toHaveAttribute("data-level", "info");
        await expect(line).toContainText("auth.c:");
        await page.getByTestId("messages-level").selectOption("error");
        await expect(page.getByTestId("message").filter({hasText: "login: user"})).toHaveCount(0);
        await page.getByTestId("messages-level").selectOption("info");
        await page.getByTestId("messages-clear").click();
        await expect(page.getByTestId("messages-none")).toBeVisible();
        await page.getByTestId("messages-close").click();
        await expect(page.getByTestId("messages")).toHaveCount(0);
    });
});

test("a refusal is listed too; who is logged in; About", async ({page, engine}) => {
    await loggedIn(page, engine.url);
    await expect(page.getByTestId("user")).toHaveText("alice · editor");
    await openProcess(page, "Simple Example");
    await expect(blocks(page)).toHaveCount(8);
    const clip = {format: "hakkas/blocks", version: 1, links: [],
        blocks: [{id: "ghost", component: "nowhere/Missing", x: 0, y: 0, initials: {}}]};
    await page.evaluate(text => {
        const data = new DataTransfer();
        data.setData("text/plain", text);
        document.dispatchEvent(new ClipboardEvent("paste", {clipboardData: data, bubbles: true}));
    }, JSON.stringify(clip));
    await expect(page.getByTestId("error")).toContainText("Unknown component: nowhere/Missing");
    await page.getByTestId("messages-toggle").click();
    const refused = page.getByTestId("message").filter({hasText: "Unknown component: nowhere/Missing"});
    await expect(refused).toHaveAttribute("data-level", "error");
    await expect(refused).toContainText("refused");

    await page.getByTestId("about-open").click();
    await expect(page.getByTestId("about-version")).not.toHaveText("—");
    await expect(page.getByTestId("about-user")).toHaveText("alice (editor)");
    await expect(page.getByTestId("about-capabilities")).toContainText("protocol:graph");
    await page.getByTestId("dialog-ok").click();
    await expect(page.getByTestId("about")).toHaveCount(0);
});
