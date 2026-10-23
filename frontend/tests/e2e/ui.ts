// Page helpers: what a user does in the frontend, and what they see.

import {expect, type Locator, type Page} from "@playwright/test";
import {EDITOR, type TestUser} from "./fixtures";

export async function login(page: Page, url: string, user: TestUser = EDITOR) {
    await page.goto(url);
    await page.fill("#user", user.user);
    await page.fill("#password", user.password);
    await page.click("button[type=submit]");
}

export async function loggedIn(page: Page, url: string, user: TestUser = EDITOR) {
    await login(page, url, user);
    await expect(page.getByTestId("logout")).toBeVisible();
}

export function processItem(page: Page, name: string): Locator {
    return page.getByTestId("process").filter({hasText: name});
}

// Opens a process and waits until its snapshot is drawn and the view has zoomed to fit it
// (06 §4): until then, blocks move under the pointer.
export async function openProcess(page: Page, name: string) {
    await processItem(page, name).click();
    await expect(processItem(page, name)).toHaveClass(/active/);
    await expect(page.getByTestId("editor")).toHaveAttribute("data-settled", "true");
}

// The blocks drawn, by their titles (block ids).
export function blocks(page: Page): Locator {
    return page.getByTestId("node");
}

export function block(page: Page, id: string): Locator {
    return blocks(page).filter({has: page.getByTestId("title").getByText(id, {exact: true})});
}

export async function blockIds(page: Page): Promise<string[]> {
    return (await page.getByTestId("title").allTextContents()).map(t => t.trim()).sort();
}

export function connections(page: Page): Locator {
    return page.getByTestId("connection");
}

// A block's position in the editor's own coordinates (its metadata x/y).
export async function blockPosition(page: Page, id: string): Promise<{ x: number, y: number }> {
    const transform = await block(page, id).evaluate(e => (e.parentElement as HTMLElement).style.transform);
    const m = /translate\(([-\d.]+)px, ([-\d.]+)px\)/.exec(transform);
    if (m === null)
        throw new Error("no position: " + transform);
    return {x: Number(m[1]), y: Number(m[2])};
}

// A block's inputs, each with its fixed value when it has one ("in1 = 12"): the value is
// drawn as a value node connected to the input (06 §4.3).
export async function inputLabels(page: Page, id: string): Promise<string[]> {
    const ports = (await block(page, id).getByTestId("input-title").allTextContents()).map(t => t.trim());
    const labels: string[] = [];
    for (const port of ports) {
        const node = valueNode(page, id, port);
        labels.push(await node.count() === 1 ? port + " = " + (await node.getByTestId("value-text").textContent())!.trim() : port);
    }
    return labels;
}

// The value node of an input (06 §4.3).
export function valueNode(page: Page, id: string, port: string): Locator {
    return page.locator(`[data-testid=value-node][data-target="${id}.${port}"]`);
}

// The selected blocks' ids (06 §4).
export async function selectedIds(page: Page): Promise<string[]> {
    return (await page.locator("[data-testid=node].selected").getByTestId("title").allTextContents())
        .map(t => t.trim()).sort();
}

// The middle of a block on the screen.
export async function blockCentre(page: Page, id: string): Promise<{ x: number, y: number }> {
    const box = (await block(page, id).boundingBox())!;
    return {x: box.x + box.width / 2, y: box.y + box.height / 2};
}
