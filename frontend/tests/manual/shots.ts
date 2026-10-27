// Helpers for the manual's screenshots (playwright.manual.config.ts).
import type {Locator, Page} from "@playwright/test";
import {fileURLToPath} from "node:url";
import {resolve} from "node:path";
import {existsSync, readdirSync, readFileSync} from "node:fs";

const OUT = fileURLToPath(new URL("../../../docs/manual/images/screenshots/", import.meta.url));

// The light theme, whatever the system prefers: screenshots look the same for everyone.
export async function lightTheme(page: Page) {
    await page.addInitScript(() => {
        try {
            localStorage.setItem("hakkas.theme", "light");
        } catch {
            // no storage: the system's theme
        }
    });
}

// Numbered markers on parts of the page, for a screenshot that the text refers to ("①").
export async function mark(page: Page, marks: { at: Locator, n: number, side?: "left" | "right" | "top" }[]) {
    const boxes = [];
    for (const m of marks) {
        const b = await m.at.first().boundingBox();
        if (b !== null)
            boxes.push({...b, n: m.n, side: m.side ?? "left"});
    }
    await page.evaluate(list => {
        for (const b of list) {
            const d = document.createElement("div");
            d.className = "manual-mark";
            d.textContent = String(b.n);
            const x = b.side === "right" ? b.x + b.width - 12 : b.side === "top" ? b.x + b.width / 2 - 14 : b.x - 14;
            const y = b.side === "top" ? b.y - 14 : b.y + Math.min(b.height / 2, 24) - 14;
            Object.assign(d.style, {position: "fixed", left: `${Math.max(2, x)}px`, top: `${Math.max(2, y)}px`,
                width: "28px", height: "28px", borderRadius: "50%", background: "#d9480f", color: "#fff",
                font: "bold 16px/28px sans-serif", textAlign: "center", zIndex: "100000",
                boxShadow: "0 0 0 2px #fff, 0 1px 4px rgba(0,0,0,.4)", pointerEvents: "none"});
            document.body.appendChild(d);
        }
    }, boxes);
}

// Saves a screenshot of the page (or of one part of it) as images/screenshots/<name>.png.
export async function shot(page: Page, name: string, part?: Locator) {
    const path = resolve(OUT, name + ".png");
    if (part !== undefined)
        await part.screenshot({path, animations: "disabled"});
    else
        await page.screenshot({path, animations: "disabled"});
}

// The device pictures' copies as the emulator publishes them (<type>.webp; describe-environment.mjs
// describes them so), for the fixture's `pictures`.
export function devicePictures(): Record<string, Buffer> {
    const data = fileURLToPath(new URL("../../../emulators/colibri/data/", import.meta.url));
    const pictures: Record<string, Buffer> = {};
    for (const kind of readdirSync(data, {withFileTypes: true}).filter(d => d.isDirectory())) {
        for (const type of readdirSync(resolve(data, kind.name))) {
            const file = resolve(data, kind.name, type, "picture.webp");
            if (existsSync(file))
                pictures[type + ".webp"] = readFileSync(file);
        }
    }
    return pictures;
}

// The vendor's picture addresses unreachable, so that the device window shows the device's
// copies: the same screenshot with or without a network.
export async function offline(page: Page) {
    await page.route("https://**/*", route => route.abort());
}

// Right-clicks a target and chooses a context menu item (06 §4.4).
export async function menu(page: Page, at: Locator, action: string, position?: { x: number, y: number }) {
    await at.click({button: "right", position});
    await page.getByTestId("context-menu").locator(`[data-action="${action}"]`).click();
}

// Saves a screenshot of the editor's content only — its blocks, value nodes and process port
// markers — with a margin, instead of the whole editor.
export async function shotOfProcess(page: Page, name: string, margin = 24) {
    const editor = (await page.getByTestId("editor").boundingBox())!;
    const parts = page.locator("[data-testid=node], [data-testid=value-node], [data-testid=port-node]");
    let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
    for (const part of await parts.all()) {
        const b = await part.boundingBox();
        if (b === null)
            continue;
        x0 = Math.min(x0, b.x);
        y0 = Math.min(y0, b.y);
        x1 = Math.max(x1, b.x + b.width);
        y1 = Math.max(y1, b.y + b.height);
    }
    const left = Math.max(editor.x + 1, x0 - margin), top = Math.max(editor.y + 1, y0 - margin);
    const right = Math.min(editor.x + editor.width - 1, x1 + margin);
    const bottom = Math.min(editor.y + editor.height - 1, y1 + margin);
    await page.screenshot({path: resolve(OUT, name + ".png"), animations: "disabled",
        clip: {x: left, y: top, width: right - left, height: bottom - top}});
}
