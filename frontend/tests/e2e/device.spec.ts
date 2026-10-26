import {expect, test} from "./fixtures";
import {loggedIn} from "./ui";

// 06 §4.15 (plan task 78): the device window — the carrier's picture with its modules in their
// slots and every channel's live value on them (environment:watch), Help links, popping out.
// The pictures are the device's own copies (plan task 77), served by hakkas.

const svg = (w: number, h: number, fill: string) =>
    `<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}"><rect width="${w}" height="${h}" fill="${fill}"/></svg>`;

const DEVICE = {
    name: "lab",
    outPorts: [{id: "slot1-ai", type: "number", addressable: true, count: 2, unit: "V"}, {id: "temp", type: "number", unit: "°C"}],
    inPorts: [{id: "slot2-dq", type: "bool"}],
    modules: [
        {slot: 0, builtin: true, type: "carrier", ports: ["temp"]},
        {slot: 1, type: "c_aiv", name: "C AIV", documentation: "https://doc.example/aiv", picture: {copy: "aiv.svg"},
            ports: ["slot1-ai"], positions: [{port: "slot1-ai", index: 1, x: 0.5, y: 0.4}]},
        {slot: 2, type: "c_ssr", name: "C SSR", ports: ["slot2-dq"], positions: [{port: "slot2-dq", x: 0.5, y: 0.5}]},
    ],
    carrier: {type: "c_8", name: "C 8", documentation: "https://doc.example/c8", picture: {copy: "c8.svg"},
        slots: 2, builtin: [{slot: 0}], comm: [],
        places: {slots: [{slot: 1, x: 0.4, y: 0.3, w: 0.1, h: 0.5}, {slot: 2, x: 0.55, y: 0.3, w: 0.1, h: 0.5}]}},
};

test.use({environment: DEVICE, pictures: {"c8.svg": svg(800, 400, "#333"), "aiv.svg": svg(50, 100, "#663")}});

const badge = (scope: { locator: (s: string) => any }, label: string) =>
    scope.locator(`[data-testid=device-badge][data-channel="${label}"]`);

test("shows the device with its live values, links to help, and pops out", async ({page, engine, context}) => {
    await loggedIn(page, engine.url);
    await page.getByTestId("env-device").click();
    const win = page.getByTestId("device-window");
    await expect(win).toBeVisible();
    await expect(page.getByTestId("device-title")).toHaveText("Device: C 8");
    // The carrier's and the module's pictures: the device's copies.
    await expect(page.getByTestId("device-board").locator("image").first()).toHaveAttribute("href", "/environment/pictures/c8.svg");
    await expect(page.locator("[data-testid=device-part][data-key=m1] image")).toHaveAttribute("href", "/environment/pictures/aiv.svg");
    // A module without a picture: a labelled box.
    await expect(page.locator("[data-testid=device-part][data-key=m2] text")).toHaveText("C SSR");
    // Help: the carrier's and a module's documentation, in a new tab.
    await expect(page.getByTestId("device-help")).toHaveAttribute("href", "https://doc.example/c8");
    await expect(page.getByTestId("device-help")).toHaveAttribute("target", "_blank");
    await expect(page.locator("[data-testid=device-part][data-key=m1]")).toHaveAttribute("href", "https://doc.example/aiv");

    // No value yet; an actuator never written is not driven.
    await expect(badge(page, "slot1-ai[1]")).toHaveAttribute("data-value", "–");
    await expect(badge(page, "slot2-dq")).toHaveClass(/none/);
    // The values as they come, on the picture and in the list of the channels without a point.
    engine.environmentSend({value: {port: "slot1-ai", index: 1, value: 3.25}});
    engine.environmentSend({value: {port: "temp", value: 21.5}});
    await expect(badge(page, "slot1-ai[1]")).toHaveAttribute("data-value", "3.25");
    await expect(badge(page, "slot1-ai[1]").locator("title")).toContainText("slot1-ai[1] (in): 3.25 V");
    const temp = page.locator("[data-testid=device-other-channel][data-channel=temp]");
    await expect(temp).toContainText("21.5 °C");
    await expect(page.locator("[data-testid=device-other-channel][data-channel='slot1-ai[2]']")).toContainText("–");

    // Popped out: the same window in one of its own, still live; closing it closes the window.
    const [popup] = await Promise.all([context.waitForEvent("page"), page.getByTestId("device-popout").click()]);
    await expect(popup.getByTestId("device-window")).toBeVisible();
    await expect(page.getByTestId("device-window")).toHaveCount(0);
    await expect(popup.getByTestId("device-popout")).toHaveCount(0);
    engine.environmentSend({value: {port: "slot1-ai", index: 1, value: 4}});
    await expect(badge(popup, "slot1-ai[1]")).toHaveAttribute("data-value", "4");
    await popup.close();
    await expect(page.getByTestId("device-window")).toHaveCount(0);
    await expect(page.getByTestId("env-device")).toHaveAttribute("aria-pressed", "false");

    // Again, with the values it has; Close closes it.
    await page.getByTestId("env-device").click();
    await expect(badge(page, "slot1-ai[1]")).toHaveAttribute("data-value", "4");
    await page.getByTestId("device-close").click();
    await expect(page.getByTestId("device-window")).toHaveCount(0);
});

test.describe("an environment without a carrier", () => {
    test.use({environment: {name: "flat", outPorts: [{id: "x", type: "number"}], inPorts: []}, pictures: undefined});

    test("has no device window", async ({page, engine}) => {
        await loggedIn(page, engine.url);
        await expect(page.getByTestId("env-name")).toHaveText("flat");
        await expect(page.getByTestId("env-device")).toHaveCount(0);
    });
});
