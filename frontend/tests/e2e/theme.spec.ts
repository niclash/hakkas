import {Page} from "@playwright/test";
import {expect, test} from "./fixtures";
import {loggedIn} from "./ui";

// 06 §4.1: pre-installed themes, chosen next to Logout, kept in the browser.

const themeHref = (page: Page) =>
    page.locator("#hakkas-theme").evaluate(l => new URL((l as HTMLLinkElement).href).pathname);
const background = (page: Page) =>
    page.evaluate(() => getComputedStyle(document.body).backgroundColor);

test("the selector lists the installed themes and switches at once", async ({page, engine}) => {
    await loggedIn(page, engine.url);
    const select = page.getByTestId("theme");
    await expect(select.locator("option")).toHaveText(["Light", "Dark", "High contrast"]);

    await select.selectOption("dark");
    await expect.poll(() => themeHref(page)).toBe("/themes/dark.css");
    await expect.poll(() => background(page)).toBe("rgb(14, 19, 24)");

    await select.selectOption("high-contrast");
    await expect.poll(() => background(page)).toBe("rgb(0, 0, 0)");
});

test("the choice is kept in the browser, also on the login screen", async ({page, engine}) => {
    await loggedIn(page, engine.url);
    await page.getByTestId("theme").selectOption("dark");
    await page.reload();
    await expect(page.locator("#user")).toBeVisible();
    expect(await themeHref(page)).toBe("/themes/dark.css");
    await expect.poll(() => background(page)).toBe("rgb(14, 19, 24)");
    await loggedIn(page, engine.url);
    await expect(page.getByTestId("theme")).toHaveValue("dark");
});

test.describe("on a system in dark mode", () => {
    test.use({colorScheme: "dark"});

    test("the default is the dark theme", async ({page, engine}) => {
        await page.goto(engine.url);
        expect(await themeHref(page)).toBe("/themes/dark.css");
        await loggedIn(page, engine.url);
        await expect(page.getByTestId("theme")).toHaveValue("dark");
    });

    test("a stored theme that is no longer installed falls back to the default at once", async ({page, engine}) => {
        await page.addInitScript(() => {
            if (sessionStorage.getItem("seeded") === null) {
                sessionStorage.setItem("seeded", "1");
                localStorage.setItem("hakkas.theme", "removed");
            }
        });
        await page.goto(engine.url);
        await expect.poll(() => themeHref(page)).toBe("/themes/dark.css");
        await expect.poll(() => background(page)).toBe("rgb(14, 19, 24)");
        await loggedIn(page, engine.url);
        await expect(page.getByTestId("theme")).toHaveValue("dark");
    });
});
