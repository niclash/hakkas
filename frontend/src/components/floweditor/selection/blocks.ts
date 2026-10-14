// The editor's selection of blocks and the moves of a dragged selection (06 §4): local view
// state, not engine state. Kept free of Rete so that it can be unit tested; selection/editor.ts
// wires it to the editor.

export type Position = { x: number, y: number };

// The selected blocks, by the editor's node ids (which a rename keeps). `changed` is told of
// every block that becomes selected or unselected.
export class BlockSelection {
    private selected = new Set<string>();
    private pressed: { id: string, accumulate: boolean } | undefined;

    constructor(private changed: (id: string, selected: boolean) => void = () => {}) {
    }

    has(id: string): boolean {
        return this.selected.has(id);
    }

    ids(): string[] {
        return [...this.selected];
    }

    // The pointer is pressed on a block. An unselected block is selected: alone, or added to
    // the selection with Ctrl/Shift (`accumulate`). A selected block stays selected with the
    // others, so that dragging it moves them all; release() settles a plain click on it.
    press(id: string, accumulate: boolean) {
        this.pressed = undefined;
        if (this.has(id)) {
            this.pressed = {id, accumulate};
            return;
        }
        if (!accumulate)
            this.clear();
        this.add([id]);
    }

    // The pointer is released after press(). A click (no move) on a selected block selects
    // only that block, or with Ctrl/Shift unselects it.
    release(moved: boolean) {
        const pressed = this.pressed;
        this.pressed = undefined;
        if (pressed === undefined || moved)
            return;
        if (pressed.accumulate)
            this.remove(pressed.id);
        else
            this.set([pressed.id]);
    }

    add(ids: string[]) {
        for (const id of ids) {
            if (!this.selected.has(id)) {
                this.selected.add(id);
                this.changed(id, true);
            }
        }
    }

    // Replaces the selection.
    set(ids: string[]) {
        const keep = new Set(ids);
        for (const id of this.ids()) {
            if (!keep.has(id))
                this.remove(id);
        }
        this.add(ids);
    }

    remove(id: string) {
        if (this.selected.delete(id))
            this.changed(id, false);
    }

    clear() {
        this.set([]);
    }

    // The block is gone (removed by an event, or the process closed): no change to report.
    forget(id: string) {
        this.selected.delete(id);
        if (this.pressed?.id === id)
            this.pressed = undefined;
    }
}

// Dragging a selection: every selected block keeps its offset from the dragged one. The
// blocks are not moved locally; their new positions go to the engine, which moves them.
export class SelectionDrag {
    private readonly start: Map<string, Position>;

    // `blocks`: the selected blocks and their positions when the drag started, the dragged
    // block among them.
    constructor(private readonly dragged: string, blocks: Map<string, Position>) {
        this.start = new Map(blocks);
    }

    // Where each block goes when the dragged block is at `position`.
    positions(position: Position): Map<string, Position> {
        const from = this.start.get(this.dragged) ?? position;
        const dx = position.x - from.x;
        const dy = position.y - from.y;
        const result = new Map<string, Position>();
        this.start.forEach((p, id) => result.set(id, {x: p.x + dx, y: p.y + dy}));
        return result;
    }
}

// Sends at most one position per block and animation frame: the latest one. flush() sends the
// pending ones at once (when the pointer is released, so the final positions are sent).
export class MoveThrottle {
    private pending = new Map<string, Position>();
    private frame: unknown = undefined;

    constructor(private readonly send: (id: string, position: Position) => void,
                private readonly schedule: (callback: () => void) => unknown =
                    callback => requestAnimationFrame(callback),
                private readonly cancel: (frame: unknown) => void =
                    frame => cancelAnimationFrame(frame as number)) {
    }

    move(id: string, position: Position) {
        this.pending.set(id, position);
        if (this.frame === undefined) {
            this.frame = this.schedule(() => {
                this.frame = undefined;
                this.flush();
            });
        }
    }

    flush() {
        if (this.frame !== undefined) {
            this.cancel(this.frame);
            this.frame = undefined;
        }
        const pending = [...this.pending];
        this.pending.clear();
        pending.forEach(([id, position]) => this.send(id, position));
    }
}
