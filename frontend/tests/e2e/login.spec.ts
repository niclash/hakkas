import {EDITOR, expect, Page, test, VIEWER} from "./fixtures";
import {loggedIn, login, processItem} from "./ui";

// 06 §2: connection and login.

test("logging in shows every block type and every process", async ({page, engine}) => {
    const other = await engine.client();
    other.send("component", "list", {});
    const count = (await other.next(m => m.command === "componentsready")).payload;

    await loggedIn(page, engine.url);
    await expect(page.getByTestId("block-type")).toHaveCount(count);
    await expect(page.locator("[data-type='math/Add2']")).toBeVisible();
    // 03 §3 (plan task 47): the comparisons are their own section; the bitwise blocks are gone.
    const compare = page.locator("details.hk-group").filter({has: page.locator("summary", {hasText: /^compare$/})});
    await expect(compare.getByTestId("block-type")).toHaveText(
        ["Greater", "Less", "Equal", "NotEqual", "LessEqual", "GreaterEqual"]);
    for (const removed of ["math/And2", "math/Or2", "math/Xor", "math/And4", "math/Or4", "math/Greater"])
        await expect(page.locator(`[data-type='${removed}']`)).toHaveCount(0);
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
    await page.reload();    // a resume too
    await expect(page.getByTestId("logout")).toBeVisible();
    expect(lines.length).toBeGreaterThan(0);
    const kept = JSON.parse(await page.evaluate(() => localStorage.getItem("hakkas.session") ?? "{}"));
    for (const line of lines) {
        expect(line).not.toContain(EDITOR.password);
        expect(line).not.toMatch(/proof|nonce|"secret"|resume/);
        expect(line).not.toContain(kept.key);
    }
});

// 06 §2, 02 §2.3.1: a reload resumes the session.
const KEPT = "hakkas.session";
const kept = (page: Page) => page.evaluate(key => localStorage.getItem(key), KEPT);

test("a reload stays logged in, again and again", async ({page, engine}) => {
    await loggedIn(page, engine.url);
    const first = await kept(page);
    expect(first).not.toBeNull();
    for (let i = 0; i < 2; i++) {
        await page.reload();
        await expect(page.getByTestId("logout")).toBeVisible();
        await expect(processItem(page, "Simple Example")).toBeVisible();
    }
    // Each resume used up its id and kept a new one; the key stays, the password is not kept.
    const now = JSON.parse((await kept(page))!);
    expect(now.id).not.toBe(JSON.parse(first!).id);
    expect(now.key).toBe(JSON.parse(first!).key);
    expect(await kept(page)).not.toContain(EDITOR.password);
});

test("a viewer's session resumes too", async ({page, engine}) => {
    await loggedIn(page, engine.url, VIEWER);
    await page.reload();
    await expect(page.getByTestId("logout")).toBeVisible();
    await expect(processItem(page, "Simple Example")).toBeVisible();
    expect(JSON.parse((await kept(page))!).user).toBe(VIEWER.user);
});

test("after a logout, a reload asks for the password", async ({page, engine}) => {
    await loggedIn(page, engine.url);
    await page.getByTestId("logout").click();
    await expect(page.locator("#user")).toBeVisible();
    expect(await kept(page)).toBeNull();
    await page.reload();
    await expect(page.locator("#user")).toBeVisible();
    await expect(page.getByTestId("login-message")).toHaveCount(0);
});

test("a session that cannot be resumed shows the login form, without an error", async ({page, engine}) => {
    await loggedIn(page, engine.url);
    // An id the engine does not know (e.g. used by another tab, or from before a restart).
    await page.evaluate(key => {
        const v = JSON.parse(localStorage.getItem(key)!);
        localStorage.setItem(key, JSON.stringify({...v, id: "AAAAAAAAAAAAAAAAAAAAAA"}));
    }, KEPT);
    await page.reload();
    await expect(page.locator("#user")).toBeVisible();
    await expect(page.locator("button[type=submit]")).toHaveText("Log in");
    await expect(page.getByTestId("login-message")).toHaveCount(0);
    expect(await kept(page)).toBeNull();
    await loggedIn(page, engine.url);
});
