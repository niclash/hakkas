import {expect, test} from "./fixtures";
import {EXAMPLE_GRAPH} from "./paths";
import {blockIds, blockPosition, blocks, connections, inputLabels, loggedIn, openProcess} from "./ui";

// 06 §3–§4: a process drawn from the graph:connect snapshot.

const EXAMPLE_IDS = ["block1", "block2", "block3", "block4", "block5", "block6", "block7", "block8"];

test("opening a process draws its blocks, connections and initials", async ({page, engine}) => {
    await loggedIn(page, engine.url);
    await openProcess(page, "Simple Example");
    await expect(blocks(page)).toHaveCount(8);
    expect(await blockIds(page)).toEqual(EXAMPLE_IDS);
    await expect(connections(page)).toHaveCount(6);
    expect(await inputLabels(page, "block8")).toEqual(["in1 = 12", "in2 = 23"]);
    expect(await inputLabels(page, "block7")).toEqual(["in1", "in2", "in3", "in4"]);
    expect(await blockPosition(page, "block8")).toEqual({x: 278, y: 478});
    expect(await blockPosition(page, "block7")).toEqual({x: 572.8, y: 365});
});

test("the view matches the engine's snapshot", async ({page, engine}) => {
    const other = await engine.client();
    const snapshot = await other.connectGraph("0826152e-3014-4c89-9380-ec4991a76af2");
    const nodes = snapshot.filter(m => m.command === "addnode").map(m => m.payload.id).sort();
    const edges = snapshot.filter(m => m.command === "addedge");

    await loggedIn(page, engine.url);
    await openProcess(page, "Simple Example");
    await expect(blocks(page)).toHaveCount(nodes.length);
    expect(await blockIds(page)).toEqual(nodes);
    await expect(connections(page)).toHaveCount(edges.length);
});

test.describe("with two processes", () => {
    const second = {
        id: "second", name: "Second",
        connections: [{src: {data: 1.5}, tgt: {process: "lin", port: "in"}}],
        blocks: {
            lin: {component: "process/Linear", metadata: {x: 100, y: 100}},
            ghost: {component: "nowhere/Missing", metadata: {x: 400, y: 100}},
        },
    };
    test.use({graphs: [second, EXAMPLE_GRAPH]});

    test("opening another process replaces the view", async ({page, engine}) => {
        await loggedIn(page, engine.url);
        await expect(page.getByTestId("process")).toHaveCount(2);
        await openProcess(page, "Simple Example");
        await expect(blocks(page)).toHaveCount(8);
        await openProcess(page, "Second");
        await expect(blocks(page)).toHaveCount(2);
        expect(await blockIds(page)).toEqual(["ghost", "lin"]);
        await expect(connections(page)).toHaveCount(0);
    });

    test("a block of an unknown type is drawn without ports", async ({page, engine}) => {
        await loggedIn(page, engine.url);
        await openProcess(page, "Second");
        expect(await inputLabels(page, "ghost")).toEqual([]);
        expect(await inputLabels(page, "lin")).toContain("in = 1.5");
    });
});
