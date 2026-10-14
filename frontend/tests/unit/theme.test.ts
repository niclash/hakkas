import {describe, expect, it} from "vitest";
import {readFileSync} from "node:fs";
import {currentTheme, defaultTheme, storedTheme, THEME_KEY, themeUrl} from "@/theme";

// 06 §4.1: pre-installed themes, the viewer's choice kept in the browser.

const media = (...on: string[]) => (query: string) => on.includes(query);
const installed = [{id: "light", name: "Light"}, {id: "dark", name: "Dark"}, {id: "high-contrast", name: "High contrast"}];
const storage = (value: string | null) => ({getItem: (key: string) => key === THEME_KEY ? value : null});

describe("theme", () => {
    it("defaults from the system's settings", () => {
        expect(defaultTheme(media())).toBe("light");
        expect(defaultTheme(media("(prefers-color-scheme: dark)"))).toBe("dark");
        expect(defaultTheme(media("(prefers-contrast: more)", "(prefers-color-scheme: dark)"))).toBe("high-contrast");
    });

    it("prefers the stored choice when it is installed", () => {
        expect(currentTheme(installed, media(), "dark")).toBe("dark");
        expect(currentTheme(installed, media("(prefers-color-scheme: dark)"), "light")).toBe("light");
        expect(currentTheme(installed, media("(prefers-color-scheme: dark)"), "removed")).toBe("dark");
        expect(currentTheme([{id: "only", name: "Only"}], media(), undefined)).toBe("only");
    });

    it("ignores a stored value that is no theme id, and storage that fails", () => {
        expect(storedTheme(storage("dark"))).toBe("dark");
        expect(storedTheme(storage("../../evil"))).toBeUndefined();
        expect(storedTheme(storage(null))).toBeUndefined();
        expect(storedTheme({getItem: () => { throw new Error("denied"); }})).toBeUndefined();
        expect(storedTheme(undefined)).toBeUndefined();
    });

    it("loads a theme from /themes/", () => {
        expect(themeUrl("dark")).toBe("/themes/dark.css");
    });
});

// Every installed theme sets every variable the styles use, so none leaves a part unstyled.
describe("installed themes", () => {
    const read = (path: string) => readFileSync(new URL("../../" + path, import.meta.url), "utf8");
    const used = new Set([...read("src/assets/hakkas.css").matchAll(/var\((--hk-[a-z0-9-]+)\)/g)].map(m => m[1]));
    const list: { id: string, name: string }[] = JSON.parse(read("public/themes/themes.json"));

    it("are listed as light, dark and high contrast", () => {
        expect(list.map(t => t.id)).toEqual(["light", "dark", "high-contrast"]);
    });

    for (const {id} of list) {
        it(`${id} defines every variable the styles use, and no other`, () => {
            const css = read(`public/themes/${id}.css`);
            const defined = new Set([...css.matchAll(/(--hk-[a-z0-9-]+)\s*:/g)].map(m => m[1]));
            expect([...used].filter(v => !defined.has(v))).toEqual([]);
            expect([...defined].filter(v => !used.has(v))).toEqual([]);
        });
    }
});
