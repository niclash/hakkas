// Chapter 13, the air handling unit (examples/ahu): the whole process, running on its emulated
// Colibri 8 with every connection watched, in a large window; then each part the chapter
// describes, cut out as a process of its own (its blocks, their values and the connections
// between them), with the signals that cross the cut as process ports named after their source or
// target: the long connections of the whole would cross every picture.
import {mkdtempSync, readFileSync, writeFileSync} from "node:fs";
import {tmpdir} from "node:os";
import {join} from "node:path";
import {expect, test} from "../e2e/fixtures";
import {loggedIn, openProcess, processItem} from "../e2e/ui";
import {lightTheme, menu, shotOfProcess} from "./shots";

const read = (path: string) => JSON.parse(readFileSync(new URL(path, import.meta.url), "utf8"));
const ahu = read("../../../examples/ahu/Air Handling Unit.process.json").processes[0];

type Ref = { block: string, port: string };
type Connection = { src: Ref | { data: unknown }, tgt: Ref };
type Port = Ref & { name: string };

// Parts whose blocks are far apart in the whole unit, placed by hand: Arrange blocks (06 §4.4)
// leaves no room for the values.
const LAYOUT: Record<string, Record<string, [number, number]>> = {
    "ahu-fans": {power_up_time: [40, 230], power_up: [420, 160], may_run: [800, 40],
        not_locked: [800, 320], fans: [1180, 120], outputs_enabled: [1180, 420]},
};

// A part of the process as a process of its own.
function cut(name: string, ids: string[]): Record<string, unknown> {
    const g = ahu.graph as { blocks: Record<string, { component: string, metadata: { x: number, y: number } }>,
        connections: Connection[], inports: Port[], outports: Port[] };
    const inside = new Set(ids);
    const x0 = Math.min(...ids.map(id => g.blocks[id].metadata.x));
    const y0 = Math.min(...ids.map(id => g.blocks[id].metadata.y));
    const at = (id: string) => LAYOUT[name]?.[id] ??
        [g.blocks[id].metadata.x - x0 + 40, g.blocks[id].metadata.y - y0 + 40];
    const blocks = Object.fromEntries(ids.map(id => [id, {component: g.blocks[id].component,
        metadata: {x: at(id)[0], y: at(id)[1]}}]));
    const inports: Port[] = g.inports.filter(p => inside.has(p.block));
    const outports: Port[] = g.outports.filter(p => inside.has(p.block));
    const unique = (list: Port[], base: string) => {
        let n = base, i = 2;
        while (list.some(p => p.name === n))
            n = `${base}_${i++}`;
        return n;
    };
    const connections: Connection[] = [];
    for (const c of g.connections) {
        const into = inside.has(c.tgt.block), from = "block" in c.src && inside.has(c.src.block);
        if (into && ("data" in c.src || from))
            connections.push(c);
        else if (into && "block" in c.src)        // from outside: an inport named after its source
            inports.push({name: unique(inports, c.src.block), ...c.tgt});
        else if (from && "block" in c.src && !outports.some(p => p.block === (c.src as Ref).block
            && p.port === (c.src as Ref).port))   // to outside: an outport named after its block
            outports.push({name: unique(outports, c.src.block), ...c.src});
    }
    return {id: "ahu-" + name, name, description: "", started: false, blocks, connections, inports, outports};
}

const PARTS: [string, string[], string[]][] = [
    ["ahu-sensors", ["outdoor_air", "supply_air", "extract_air", "exhaust_air", "frost_water", "room_air"],
        ["outdoor_temperature", "supply_temperature", "extract_temperature", "exhaust_temperature",
            "frost_temperature", "room_temperature"]],
    ["ahu-operation", ["workdays", "work_hours", "schedule", "extended", "extended_wait", "extended_timer", "running"], []],
    ["ahu-seasons", ["heating_months", "september", "from_15th", "may", "to_15th", "late_september", "early_may",
        "half_months", "heating_season", "cooling_season", "any_season", "neither_season"], []],
    ["ahu-setpoint", ["supply_curve", "room_compensation", "supply_setpoint"], []],
    ["ahu-regulators", ["heating_regulator_on", "heating_regulator", "exchanger_regulator_on", "exchanger_regulator",
        "cooling_regulator_on", "cooling_regulator", "exchanger_manual", "heating_demand", "heating_valve_volts",
        "exchanger_volts", "cooling_valve_volts"], ["heating_valve", "heat_exchanger", "cooling_valve"]],
    ["ahu-frost", ["frost_negated", "frost", "frost_confirmed", "frost_cleared", "frost_acknowledge", "frost_alarm",
        "frost_lock", "frost_regulator"], []],
    ["ahu-fans", ["power_up_time", "power_up", "may_run", "not_locked", "fans", "outputs_enabled"], []],
    ["ahu-pumps", ["exercise_day", "exercise_time", "exercise", "heating_needed", "heating_or_frost", "heating_pump",
        "cooling_needed", "cooling_pump"], []],
];
const dir = mkdtempSync(join(tmpdir(), "hakkas-ahu-parts-"));
const parts = PARTS.map(([name, ids]) => {
    const file = join(dir, name + ".json");
    writeFileSync(file, JSON.stringify(cut(name, ids)));
    return file;
});

// The parts as paths: a second object would make Playwright read the list as [value, options].
test.use({graphs: [ahu.graph, ...parts], environment: read("environments/ahu.json"),
    viewport: {width: 3800, height: 2700}});

test("the whole unit, running", async ({page, engine}) => {
    test.setTimeout(60000);
    await lightTheme(page);
    const other = await engine.client();
    for (const b of ahu.bindings.bindings)
        await other.command("environment", "bind", {graph: ahu.graph.id, ...b});
    other.send("network", "start", {graph: ahu.graph.id});
    await other.next(m => m.command === "started");
    const temperatures: [string, number, number][] = [["slot1-ai", 1, 2.4], ["slot1-ai", 2, 19.2],
        ["slot2-ai", 1, 21.8], ["slot2-ai", 2, 6.3], ["slot3-ai", 1, 31.2], ["slot3-ai", 2, 20.6]];
    for (const [port, index, value] of temperatures)
        engine.environmentSend({value: {port, index, value}});
    await loggedIn(page, engine.url);
    await openProcess(page, "Air Handling Unit");
    await expect(processItem(page, "Air Handling Unit").getByTestId("process-state")).toHaveClass(/running/);
    const editor = page.getByTestId("editor");
    const box = (await editor.boundingBox())!;
    await menu(page, editor, "watch-all", {x: 30, y: box.height - 30});
    await page.waitForTimeout(11000);       // past the start delay: the fans run, the regulators work
    await shotOfProcess(page, "ahu-process");
});

test("its parts", async ({page, engine}) => {
    test.setTimeout(90000);
    await lightTheme(page);
    await page.setViewportSize({width: 1800, height: 1100});
    await loggedIn(page, engine.url);
    // The setpoint's blocks are far apart in the whole unit: laid out by Arrange blocks (06 §4.4)
    // first. The others keep the unit's layout or LAYOUT's, which leave room for the values.
    const editor = page.getByTestId("editor");
    for (const [name] of PARTS) {
        await openProcess(page, name);
        if (name === "ahu-setpoint") {
            const box = (await editor.boundingBox())!;
            const corner = {x: 20, y: box.height - 20};
            await menu(page, editor, "arrange", corner);
            await page.waitForTimeout(2000);
            await menu(page, editor, "zoom-fit", corner);
        }
        await shotOfProcess(page, name);
    }
});


