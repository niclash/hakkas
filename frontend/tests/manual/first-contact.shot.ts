// Chapter 3, First contact: the login screen and a tour of the main screen, with the air
// handling unit (examples/ahu) on its Colibri 8.
import {readFileSync} from "node:fs";
import {expect, test} from "../e2e/fixtures";
import {block, login, openProcess} from "../e2e/ui";
import {lightTheme, mark, shot} from "./shots";

const ahu = JSON.parse(readFileSync(new URL("../../../examples/ahu/Air Handling Unit.process.json", import.meta.url),
    "utf8")).processes[0].graph;
const environment = JSON.parse(readFileSync(new URL("environments/ahu.json", import.meta.url), "utf8"));

test.use({graphs: [ahu], environment});

test("the login screen", async ({page, engine}) => {
    await lightTheme(page);
    await page.goto(engine.url);
    await expect(page.locator("#user")).toBeVisible();
    await page.fill("#user", "alice");
    await shot(page, "login");
});

test("the main screen", async ({page, engine}) => {
    await lightTheme(page);
    await login(page, engine.url);
    await expect(page.getByTestId("logout")).toBeVisible();
    await openProcess(page, "Air Handling Unit");
    await block(page, "heating_regulator").getByTestId("title").click();
    await expect(page.getByTestId("inspector-type")).toBeVisible();
    // The block type families collapsed, so that the Environment panel below them shows.
    await page.locator("details.hk-group").evaluateAll(groups => groups.forEach(g => g.removeAttribute("open")));
    await expect(page.getByTestId("env-panel")).toBeInViewport();
    await mark(page, [
        {at: page.getByTestId("user"), n: 1},
        {at: page.getByTestId("process"), n: 2},
        {at: page.getByTestId("palette-search"), n: 3},
        {at: page.getByTestId("env-panel"), n: 4},
        {at: page.getByTestId("editor"), n: 5, side: "top"},
        {at: page.getByTestId("inspector"), n: 6},
    ]);
    await shot(page, "main-screen");
});
