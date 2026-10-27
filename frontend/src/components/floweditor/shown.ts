// Whether the editor is on the page and shown: hidden under the process map (06 §4.17), its
// keys (Delete, Ctrl+Z, Ctrl+C, …) are not its own.
export function shown(container: HTMLElement): boolean {
    return container.isConnected && container.offsetParent !== null;
}
