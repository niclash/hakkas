// Chapter 12, the heating circuit (examples/heating-circuit): uploaded with Upload…, its
// composite type looked into, and running on its emulated Colibri 8 with every connection watched.
import {fileURLToPath} from "node:url";
import {readFileSync} from "node:fs";
import {expect, test} from "../e2e/fixtures";
import {block, loggedIn, processItem} from "../e2e/ui";
import {devicePictures, lightTheme, menu, offline, shot, shotOfProcess} from "./shots";

const FILE = fileURLToPath(new URL("../../../examples/heating-circuit/Heating Circuit.process.json", import.meta.url));
const environment = JSON.parse(readFileSync(new URL("environments/heating.json", import.meta.url), "utf8"));

test.use({graphs: [], environment, pictures: devicePictures(), viewport: {width: 1800, height: 1000}});

test("the heating circuit", async ({page, engine}) => {
    test.setTimeout(90000);
    await lightTheme(page);
    await offline(page);
    await loggedIn(page, engine.url);
    const editor = page.getByTestId("editor");
    const empty = async () => ({x: 30, y: (await editor.boundingBox())!.height - 30});

    // Uploaded: the process, its bindings and its composite type.
    await page.getByTestId("process-upload-file").setInputFiles(FILE);
    const dialog = page.getByTestId("transfer-dialog");
    await expect(dialog.getByTestId("transfer-type")).toHaveCount(1);
    await shot(page, "heating-upload", dialog);
    await page.getByTestId("transfer-upload").click();
    await page.getByTestId("transfer-close").click();
    await expect(processItem(page, "Heating Circuit")).toHaveClass(/active/);
    await expect(editor).toHaveAttribute("data-settled", "true");

    // Started, with values on every connection.
    await processItem(page, "Heating Circuit").getByTestId("process-run").click();
    for (const [index, value] of [[1, 2.4], [2, 41.6]])
        engine.environmentSend({value: {port: "slot1-ai", index, value}});
    await menu(page, editor, "watch-all", await empty());
    await page.waitForTimeout(1500);
    await shotOfProcess(page, "heating-process");

    // The curve's points in the inspector.
    await block(page, "curve").getByTestId("title").click();
    await shot(page, "heating-curve", page.getByTestId("inspector"));
    await page.keyboard.press("Escape");

    // The composite type, opened from the palette.
    const entry = page.locator("[data-testid=block-type][data-type='heating/ShuntGroup']");
    await entry.locator("xpath=..").getByTestId("open-type").click();
    await expect(page.getByTestId("type-banner")).toBeVisible();
    await expect(editor).toHaveAttribute("data-settled", "true");
    await shot(page, "heating-shunt-group", editor);
});
