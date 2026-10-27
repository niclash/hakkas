// Chapter 11, the process map: an outdoor station's temperature linked into the air handling unit
// (examples/ahu), beside the thermostat (examples/thermostat).
import {mkdtempSync, readFileSync, writeFileSync} from "node:fs";
import {tmpdir} from "node:os";
import {join} from "node:path";
import {expect, test} from "../e2e/fixtures";
import {loggedIn} from "../e2e/ui";
import {lightTheme, shot} from "./shots";

const read = (path: string) => JSON.parse(readFileSync(new URL(path, import.meta.url), "utf8"));
const ahu = read("../../../examples/ahu/Air Handling Unit.process.json").processes[0].graph;
const thermostat = read("../../../examples/thermostat/Thermostat.process.json").processes[0].graph;
const outdoor = {
    id: "outdoor-station", name: "Outdoor station", description: "The outdoor temperature, for every process that needs it",
    blocks: {outdoor: {component: "process/Deadband", metadata: {x: 40, y: 40}}},
    connections: [{src: {data: 0.1}, tgt: {block: "outdoor", port: "threshold"}}],
    inports: [{name: "sensor", block: "outdoor", port: "in"}],
    outports: [{name: "outdoor_temperature", block: "outdoor", port: "out"}],
};
const dir = mkdtempSync(join(tmpdir(), "hakkas-map-shot-"));
const ahuFile = join(dir, "ahu.json");
writeFileSync(ahuFile, JSON.stringify(ahu));

// A path second: two objects first would be read as Playwright's [value, options].
test.use({graphs: [outdoor, ahuFile, thermostat], viewport: {width: 1500, height: 860}});

test("the process map", async ({page, engine}) => {
    await lightTheme(page);
    const other = await engine.client();
    await other.command("link", "add", {src: {graph: "outdoor-station", port: "outdoor_temperature"},
        tgt: {graph: ahu.id, port: "outdoor_temperature"}});
    await loggedIn(page, engine.url);
    await page.getByTestId("process-map").click();
    await expect(page.getByTestId("map")).toHaveAttribute("data-settled", "true");
    await shot(page, "process-map");
});
