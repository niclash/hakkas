// Live values (06 §4.7): the inputs the client watches, sent whole as network:edges (a request
// replaces the previous set, 02 §4.2), and how a value is shown. Kept free of Vue and Rete so
// that it can be unit tested.

export interface Point {
    node: string;
    port: string;
}

export interface Watch {
    src: Point;   // what feeds the input: an output, or the input itself (a fixed value)
    tgt: Point;   // the input watched
}

// An input's key, for the maps below.
export function pointKey(p: Point): string {
    return JSON.stringify([p.node, p.port]);
}

export class Watches {
    // The inputs watched one by one (a connection, a block's inputs), by their key.
    private explicit = new Map<string, Watch>();
    // "Watch all": every connection of the process, also those added later.
    all = false;

    has(tgt: Point): boolean {
        return this.explicit.has(pointKey(tgt));
    }

    add(watch: Watch) {
        this.explicit.set(pointKey(watch.tgt), watch);
    }

    remove(tgt: Point) {
        this.explicit.delete(pointKey(tgt));
    }

    clear() {
        this.explicit.clear();
        this.all = false;
    }

    // The block was renamed (graph:renamenode): its watches follow it.
    renameBlock(from: string, to: string) {
        const renamed = [...this.explicit.values()].map(w => ({
            src: w.src.node === from ? {...w.src, node: to} : w.src,
            tgt: w.tgt.node === from ? {...w.tgt, node: to} : w.tgt,
        }));
        this.explicit = new Map(renamed.map(w => [pointKey(w.tgt), w]));
    }

    // The block was removed: its inputs' watches end (the engine ends them too, 02 §4.2).
    removeBlock(id: string) {
        for (const [key, w] of this.explicit) {
            if (w.tgt.node === id)
                this.explicit.delete(key);
        }
    }

    // The set to send: the explicit watches, then (with "watch all") every connection not
    // already among them. An explicit watch keeps its `src`, unless a connection now feeds it.
    request(connections: Watch[]): Watch[] {
        const feeding = new Map(connections.map(c => [pointKey(c.tgt), c]));
        const list = [...this.explicit.values()].map(w => feeding.get(pointKey(w.tgt)) ?? w);
        if (this.all) {
            for (const c of connections) {
                if (!this.explicit.has(pointKey(c.tgt)))
                    list.push(c);
            }
        }
        return list;
    }
}

// A live value as shown, from the string the engine sends: a number with at most four
// decimals, a bool as it is, text quoted and cut to 24 characters.
export function formatLive(portType: string, data: string): string {
    if (portType === "bool" || data === "")
        return data;
    if (portType === "string")
        return "“" + (data.length > 24 ? data.substring(0, 23) + "…" : data) + "”";
    const n = Number(data);
    if (!Number.isFinite(n))
        return data;
    return String(Math.round(n * 1e4) / 1e4);
}
