// Helpers for the manual's screenshots (playwright.manual.config.ts).
import type {Locator, Page} from "@playwright/test";
import {fileURLToPath} from "node:url";
import {resolve} from "node:path";

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
