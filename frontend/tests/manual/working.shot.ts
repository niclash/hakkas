// Part III, Working with hakkas (Chapters 5-10): the editor's menus and selection, a composite
// type, the process list, the environment, the device window, the mapping and upload dialogs, and
// a viewer's view. The thermostat (examples/thermostat) and the air handling unit (examples/ahu)
// on their emulated Colibri 8s.
import {readFileSync, writeFileSync} from "node:fs";
import {fileURLToPath} from "node:url";
import {expect, test, VIEWER} from "../e2e/fixtures";
import {block, loggedIn, openProcess, processItem} from "../e2e/ui";
import {devicePictures, lightTheme, menu, offline, shot, shotAround, shotOfProcess} from "./shots";

const read = (path: string) => JSON.parse(readFileSync(new URL(path, import.meta.url), "utf8"));
const thermostat = read("../../../examples/thermostat/Thermostat.process.json").processes[0];
const ahu = read("../../../examples/ahu/Air Handling Unit.process.json").processes[0];
const SHUNT_GROUP_FILE = fileURLToPath(new URL("../../../examples/shunt-group.json", import.meta.url));
const THERMOSTAT_FILE = fileURLToPath(new URL("../../../examples/thermostat/Thermostat.process.json", import.meta.url));

test.describe("the editor", () => {
    test.use({graphs: [thermostat.graph], environment: read("environments/thermostat.json")});

    test("menus, the marquee and a composite type", async ({page, engine}) => {
        await lightTheme(page);
        await loggedIn(page, engine.url);
        await openProcess(page, "Thermostat");
        const editor = page.getByTestId("editor");
        const contextMenu = page.getByTestId("context-menu");
        const box = (await editor.boundingBox())!;
        const empty = {x: 40, y: box.height - 60};

        await editor.click({button: "right", position: empty});
        await shotAround(page, "menu-canvas", contextMenu, {x: 40, y: 40});
        await page.keyboard.press("Escape");
        // Closer, for the block's and an input's menus.
        const near = (await block(page, "occupied").boundingBox())!;
        await page.mouse.move(near.x + near.width / 2, near.y + near.height / 2);
        await page.keyboard.down("Control");
        for (let i = 0; i < 5; i++)
            await page.mouse.wheel(0, -100);
        await page.keyboard.up("Control");
        await block(page, "setpoint").getByTestId("title").click({button: "right"});
        await shotAround(page, "menu-block", contextMenu, {x: 220, y: 60});
        await page.keyboard.press("Escape");
        await block(page, "workdays").getByTestId("input-sunday_first").getByTestId("input-title").click({button: "right"});
        await shotAround(page, "menu-input", contextMenu, {x: 240, y: 60});
        await page.keyboard.press("Escape");
        await menu(page, editor, "add-block", empty);
        await page.keyboard.type("lim");
        await shotAround(page, "menu-add-block", contextMenu, {x: 40, y: 40});
        await page.keyboard.press("Escape");
        await menu(page, editor, "zoom-fit", empty);

        // The marquee around the schedule's three blocks, while it is drawn.
        const a = (await block(page, "office_hours").boundingBox())!;
        const b = (await block(page, "occupied").boundingBox())!;
        const w = (await block(page, "workdays").boundingBox())!;
        await page.mouse.move(a.x - 20, a.y - 20);
        await page.mouse.down();
        await page.mouse.move(b.x + b.width + 20, w.y + w.height + 20, {steps: 10});
        await shotOfProcess(page, "marquee");
        await page.mouse.up();

        // A composite type from them.
        await menu(page, block(page, "occupied").getByTestId("title"), "create-composite");
        const dialog = page.getByTestId("composite-dialog");
        await page.getByTestId("composite-type").fill("user/OfficeHours");
        await page.getByTestId("composite-description").fill("True during office hours");
        for (const port of ["office_hours.range", "workdays.days"]) {
            const row = dialog.locator(`[data-testid=composite-port][data-port="${port}"]`);
            await row.getByTestId("composite-expose").check();
            await row.getByTestId("composite-name").fill(port.endsWith("range") ? "hours" : "days");
        }
        await dialog.locator("[data-testid=composite-port][data-port='occupied.out']").getByTestId("composite-name").fill("occupied");
        await shot(page, "composite-dialog", dialog);
        await page.getByTestId("dialog-ok").click();
        await expect(dialog).toHaveCount(0);
        const entry = page.locator("[data-testid=block-type][data-type='user/OfficeHours']");
        await expect(entry).toBeVisible();
        await entry.locator("xpath=..").getByTestId("open-type").click();
        await expect(page.getByTestId("type-banner")).toBeVisible();
        await page.mouse.move(box.x + box.width / 2, box.y + 200);
        await expect(page.getByTestId("editor")).toHaveAttribute("data-settled", "true");
        await shot(page, "composite-open");
    });
});

test.describe("processes and the environment", () => {
    // A path second: two objects first would be read as Playwright's [value, options].
    test.use({graphs: [thermostat.graph, SHUNT_GROUP_FILE, ahu.graph], environment: read("environments/ahu.json"),
        pictures: devicePictures()});

    test("the list, the panel, the device window and the dialogs", async ({page, engine}) => {
        await lightTheme(page);
        await offline(page);
        const other = await engine.client();
        for (const b of ahu.bindings.bindings)
            await other.command("environment", "bind", {graph: ahu.graph.id, ...b});
        other.send("network", "start", {graph: ahu.graph.id});
        await other.next(m => m.command === "started");
        const temperatures: [string, number, number][] = [["slot1-ai", 1, 2.4], ["slot1-ai", 2, 19.7],
            ["slot2-ai", 1, 21.8], ["slot2-ai", 2, 6.3], ["slot3-ai", 1, 31.2], ["slot3-ai", 2, 21.1]];
        for (const [port, index, value] of temperatures)
            engine.environmentSend({value: {port, index, value}});
        await loggedIn(page, engine.url);
        await openProcess(page, "Air Handling Unit");
        await expect(processItem(page, "Air Handling Unit").getByTestId("process-state")).toHaveClass(/running/);
        await shot(page, "process-list", page.locator(".hk-panel").filter({has: page.getByTestId("process-new")}));

        // The Environment panel, its first module open.
        await page.locator("details.hk-group").evaluateAll(groups => groups.forEach(g => g.removeAttribute("open")));
        await page.getByTestId("env-module").nth(1).locator("summary").click();
        await shot(page, "environment-panel", page.getByTestId("env-panel"));

        await page.getByTestId("env-device").click();
        await expect(page.getByTestId("device-window")).toBeVisible();
        await page.waitForTimeout(1500);      // the pictures, the values
        await shot(page, "device-window", page.getByTestId("device-window"));
        await page.getByTestId("env-device").click();

        // The thermostat's bindings file, written for another environment: the mapping dialog.
        await openProcess(page, "Thermostat");
        const file = test.info().outputPath("Thermostat.bindings.json");
        writeFileSync(file, JSON.stringify(thermostat.bindings));
        await page.getByTestId("env-file").setInputFiles(file);
        const mapping = page.getByTestId("mapping-dialog");
        await expect(mapping).toBeVisible();
        await shot(page, "mapping-dialog", mapping);
        await page.getByTestId("mapping-cancel").click();

        // Upload: the thermostat's file onto a controller that has it.
        await page.getByTestId("process-upload-file").setInputFiles(THERMOSTAT_FILE);
        const transfer = page.getByTestId("transfer-dialog");
        await expect(transfer.getByTestId("transfer-process")).toHaveCount(1);
        await shot(page, "upload-dialog", transfer);
        await page.getByTestId("transfer-cancel").click();
    });
});

test.describe("a viewer", () => {
    test.use({graphs: [thermostat.graph]});

    test("the top bar and About", async ({page, engine}) => {
        await lightTheme(page);
        await loggedIn(page, engine.url, VIEWER);
        await openProcess(page, "Thermostat");
        await shot(page, "viewer");
        await page.getByTestId("about-open").click();
        await shot(page, "about", page.locator(".hk-dialog"));
    });
});
