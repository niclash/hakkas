// Themes (06 §4.1): pre-installed stylesheets in /themes/, listed by /themes/themes.json, each
// setting the variables of src/assets/hakkas.css. The choice is the viewer's, kept in this
// browser (localStorage), not in the engine. index.html applies it before the first paint
// with the same rules as here.

export interface ThemeInfo {
    id: string;
    name: string;
}

export const THEME_KEY = "hakkas.theme";
const THEME_LINK = "hakkas-theme";
const VALID_ID = /^[a-z0-9-]{1,32}$/;

// Without a choice: high contrast if the system asks for more contrast, else dark or light.
export function defaultTheme(matches: (query: string) => boolean): string {
    if (matches("(prefers-contrast: more)"))
        return "high-contrast";
    return matches("(prefers-color-scheme: dark)") ? "dark" : "light";
}

export function themeUrl(id: string): string {
    return "/themes/" + id + ".css";
}

// The chosen theme, or undefined (none chosen, storage unavailable, or an invalid value).
export function storedTheme(storage: Pick<Storage, "getItem"> | undefined = safeStorage()): string | undefined {
    try {
        const id = storage?.getItem(THEME_KEY);
        return id && VALID_ID.test(id) ? id : undefined;
    } catch {
        return undefined;
    }
}

// The theme in use: the stored choice if it is installed, else the default.
export function currentTheme(installed: ThemeInfo[], matches: (query: string) => boolean,
                             stored = storedTheme()): string {
    const fallback = defaultTheme(matches);
    if (stored !== undefined && installed.some(t => t.id === stored))
        return stored;
    return installed.some(t => t.id === fallback) ? fallback : (installed[0]?.id ?? fallback);
}

export async function installedThemes(): Promise<ThemeInfo[]> {
    const response = await fetch("/themes/themes.json", {cache: "no-cache"});
    if (!response.ok)
        throw new Error("themes.json: " + response.status);
    const list = await response.json();
    return (Array.isArray(list) ? list : []).filter(
        (t: any) => typeof t?.id === "string" && VALID_ID.test(t.id) && typeof t?.name === "string");
}

// Switches the page's stylesheet and remembers the choice.
export function applyTheme(id: string) {
    if (!VALID_ID.test(id))
        return;
    const link = document.getElementById(THEME_LINK) as HTMLLinkElement | null;
    if (link !== null)
        link.href = themeUrl(id);
    document.documentElement.dataset.theme = id;
    try {
        safeStorage()?.setItem(THEME_KEY, id);
    } catch {
        // Storage refused (e.g. a private window): the choice lasts for this page only.
    }
}

// At startup: replaces a stored theme that is no longer installed (index.html has already
// applied it, and its stylesheet is missing) by the default. Returns the installed themes.
export async function settleTheme(): Promise<ThemeInfo[]> {
    const themes = await installedThemes();
    const id = currentTheme(themes, mediaMatches);
    if (document.documentElement.dataset.theme !== id)
        applyTheme(id);
    return themes;
}

export function mediaMatches(query: string): boolean {
    return typeof matchMedia === "function" && matchMedia(query).matches;
}

function safeStorage(): Storage | undefined {
    try {
        return typeof localStorage === "undefined" ? undefined : localStorage;
    } catch {
        return undefined;
    }
}
