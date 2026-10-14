// A block rename sent from the inspector and not answered yet (06 §4.2). The engine answers
// with the graph:renamenode event, or refuses with a network:error that names no command
// (02 §3.4.1); while a rename is pending, the refusals it can cause are taken as its own and
// shown at the name field instead of with the other errors.

export class PendingRename {
    private pending: { from: string, to: string } | undefined;

    sent(from: string, to: string) {
        this.pending = {from, to};
    }

    get active(): boolean {
        return this.pending !== undefined;
    }

    // The selected block's id is now `id`: the rename is done when it is the new name, and
    // moot when it is neither name (another block, or renamed by someone else).
    shown(id: string | undefined) {
        if (this.pending !== undefined && id !== this.pending.from)
            this.pending = undefined;
    }

    // Whether a network:error refuses the pending rename; it is then no longer pending.
    refusal(message: string): boolean {
        const p = this.pending;
        if (p === undefined)
            return false;
        const ours = message === "Node exists: " + p.to
            || message === "Invalid id"
            || message === "Unknown node: " + p.from
            || message === "Not permitted: graph:renamenode";
        if (ours)
            this.pending = undefined;
        return ours;
    }

    cancel() {
        this.pending = undefined;
    }
}
