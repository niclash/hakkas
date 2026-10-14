// The open process or block type in the page's address (06 §2.1): "/?process=<id>" or
// "/?type=<name>", so that it can be bookmarked, a reload keeps it and the browser's back and
// forward buttons move between what was open. In the query rather than the path: the web
// server takes a path's "%2F" and "." as a file name's (05 §3), and graph ids may hold both (a
// type's name always has a "/").

export interface Opened {
    kind: "process" | "type";
    id: string;
}

export function addressFor(opened: Opened | undefined): string {
    return opened === undefined ? "/" : "/?" + new URLSearchParams({[opened.kind]: opened.id}).toString();
}

// What the address's query names, if anything.
export function parseAddress(search: string): Opened | undefined {
    const q = new URLSearchParams(search);
    const process = q.get("process"), type = q.get("type");
    if (process)
        return {kind: "process", id: process};
    if (type)
        return {kind: "type", id: type};
    return undefined;
}

// The page's title: the product, and what is open.
export function titleFor(name: string | undefined): string {
    return name ? name + " — Hakkas" : "Hakkas";
}
