import {EDITOR, expect, test, VIEWER} from "./fixtures";
import {loggedIn, login, processItem} from "./ui";

// 06 §2: connection and login.

test("logging in shows every block type and every process", async ({page, engine}) => {
    const other = await engine.client();
    other.send("component", "list", {});
    const count = (await other.next(m => m.command === "componentsready")).payload;

    await loggedIn(page, engine.url);
    await expect(page.getByTestId("block-type")).toHaveCount(count);
    await expect(page.locator("[data-type='math/Add2']")).toBeVisible();
    await expect(page.getByTestId("process")).toHaveCount(1);
    await expect(processItem(page, "Simple Example")).toBeVisible();
});

test("a wrong password is refused, and a retry succeeds", async ({page, engine}) => {
    await login(page, engine.url, {...EDITOR, password: "wrong"});
    await expect(page.getByTestId("login-message")).toHaveText("Login failed");
    await expect(page.locator("button[type=submit]")).toHaveText("Log in");
    await loggedIn(page, engine.url);
});

test("an unknown user is refused like a wrong password", async ({page, engine}) => {
    await login(page, engine.url, {...EDITOR, user: "mallory"});
    await expect(page.getByTestId("login-message")).toHaveText("Login failed");
});

test("every role can log in", async ({page, engine}) => {
    await loggedIn(page, engine.url, VIEWER);
    await expect(processItem(page, "Simple Example")).toBeVisible();
});

test("logging out returns to the login screen", async ({page, engine}) => {
    await loggedIn(page, engine.url);
    await page.getByTestId("logout").click();
    await expect(page.locator("#user")).toBeVisible();
    await expect(page.getByTestId("login-message")).toHaveCount(0);
});

test.describe("with a short session", () => {
    test.use({config: {auth: {idle_timeout_s: 2}}});

    test("an expired session returns to the login screen", async ({page, engine}) => {
        await loggedIn(page, engine.url);
        await expect(page.locator("#user")).toBeVisible({timeout: 6000});
    });
});

test("a lost connection says so on the login screen", async ({page, engine}) => {
    await loggedIn(page, engine.url);
    await engine.stop();
    await expect(page.getByTestId("login-message")).toHaveText("The connection to the server was lost");
});

test("nothing secret reaches the browser's console", async ({page, engine}) => {
    const lines: string[] = [];
    page.on("console", m => lines.push(m.text()));
    await loggedIn(page, engine.url);
    expect(lines.length).toBeGreaterThan(0);
    for (const line of lines) {
        expect(line).not.toContain(EDITOR.password);
        expect(line).not.toMatch(/proof|nonce|"secret"/);
    }
});
