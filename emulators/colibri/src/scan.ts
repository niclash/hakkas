// The slot scan cycle (spec §4) and the write queues (§4.1): one slot served per cycle time, on
// deadlines so the cycle does not drift; the writes from hakkas queued per module and applied at
// its turn; the counters of the control protocol's `stats` (§3.6).
import type {Logger} from './log.js';
import type {Clock, InputChannel, OutputChannel, Slot, System} from './system.js';

// What one turn did: the outputs it applied (in order, one entry per write) and the inputs
// whose value changed, to send to hakkas and the clients.
export interface Turn {
    slot: Slot;
    applied: OutputChannel[];
    changed: InputChannel[];
}

export interface PartStats {
    queue: number;
    queueMax: number;
    dropped: number;
}

export interface OutputStats {
    received: number;
    queued: number;
    dropped: number;
}

export interface InputStats {
    sent: number;
}

export interface Stats {
    parts: Record<string, PartStats>;
    channels: Record<string, OutputStats | InputStats>;
}

export type WriteResult = 'queued' | 'dropped';

interface Write {
    ch: OutputChannel;
    value: number | boolean;
}

// The warnings about skipped turns come at most this often (ms).
const SKIP_WARNING_MS = 60_000;

export class Scanner {
    private cycleMs: number;
    private queueSize: number;
    private turnIndex = 0;              // the next turn; it serves slot turnIndex mod slots
    private due: number;                // when the next turn is due (ms)
    private readonly queues: Write[][];
    private readonly parts: PartStats[];
    private readonly outputs = new Map<OutputChannel, OutputStats>();
    private readonly inputs = new Map<InputChannel, InputStats>();
    private skipped = 0;                // turns skipped since the last warning
    private warned = -Infinity;
    private timer: NodeJS.Timeout | undefined;

    constructor(readonly system: System, settings: {cycleMs: number, queueSize: number},
                private readonly clock: Clock, private readonly log: Logger,
                private readonly onTurn: (turn: Turn) => void = () => {}) {
        this.cycleMs = settings.cycleMs;
        this.queueSize = settings.queueSize;
        this.due = clock();
        this.queues = system.slots.map(() => []);
        this.parts = system.slots.map(() => ({queue: 0, queueMax: 0, dropped: 0}));
        for (const slot of system.slots) {
            for (const ch of slot.outputs)
                this.outputs.set(ch, {received: 0, queued: 0, dropped: 0});
            for (const ch of slot.inputs)
                this.inputs.set(ch, {sent: 0});
        }
    }

    // Serves the turns that are due by now. Turns more than a whole cycle behind are skipped, so
    // a late scanner serves each slot once and then keeps time again.
    poll(): void {
        const now = this.clock();
        const behind = Math.floor((now - this.due) / this.cycleMs) + 1;     // turns due
        const slots = this.system.slots.length;
        if (behind > slots) {
            const skip = behind - slots;
            this.turnIndex += skip;
            this.due += skip * this.cycleMs;
            this.skipped += skip;
        }
        if (this.skipped > 0 && now - this.warned >= SKIP_WARNING_MS) {
            this.log.warn(`scan cycle: ${this.skipped} turns skipped, more than a whole cycle late`);
            this.warned = now;
            this.skipped = 0;
        }
        while (this.due <= now) {
            this.serve(this.turnIndex % slots);
            this.turnIndex++;
            this.due += this.cycleMs;
        }
    }

    // Serves one slot (spec §4): applies its queued writes in order, then samples its inputs.
    private serve(index: number): void {
        const slot = this.system.slots[index]!;
        const queue = this.queues[index]!;
        this.queues[index] = [];
        this.parts[index]!.queue = 0;
        for (const w of queue)
            this.system.applyOutput(w.ch, w.value);
        const changed = this.system.sample(index);
        for (const ch of changed)
            this.inputs.get(ch)!.sent++;
        this.onTurn({slot, applied: queue.map(w => w.ch), changed});
    }

    // Queues a value hakkas wrote to an output channel, for its module's next turn (§4.1). A full
    // queue drops it.
    write(ch: OutputChannel, value: number | boolean): WriteResult {
        const stats = this.outputs.get(ch);
        if (stats === undefined)
            throw new RangeError(`not an output of this system: ${ch.key}`);
        const part = this.parts[ch.slot]!;
        const queue = this.queues[ch.slot]!;
        stats.received++;
        if (queue.length >= this.queueSize) {
            stats.dropped++;
            part.dropped++;
            return 'dropped';
        }
        queue.push({ch, value});
        stats.queued++;
        part.queue = queue.length;
        part.queueMax = Math.max(part.queueMax, part.queue);
        return 'queued';
    }

    // A new cycle time takes effect from the next turn: the turn after it is due a new cycle time
    // after that one.
    setCycleMs(ms: number): void {
        this.cycleMs = ms;
    }

    // A smaller queue keeps the writes it holds; new ones are dropped until it is below the size.
    setQueueSize(size: number): void {
        this.queueSize = size;
    }

    stats(): Stats {
        const out: Stats = {parts: {}, channels: {}};
        this.system.slots.forEach((slot, i) => {
            if (slot.module !== undefined)
                out.parts[slot.part] = {...this.parts[i]!};
        });
        for (const [ch, s] of this.outputs)
            out.channels[ch.key] = {...s};
        for (const [ch, s] of this.inputs)
            out.channels[ch.key] = {...s};
        return out;
    }

    // Sets every counter to 0 (control protocol §4.6); a queue's current length stays what it is.
    resetStats(): void {
        this.parts.forEach((p, i) => {
            p.dropped = 0;
            p.queueMax = this.queues[i]!.length;
            p.queue = this.queues[i]!.length;
        });
        for (const s of this.outputs.values())
            Object.assign(s, {received: 0, queued: 0, dropped: 0});
        for (const s of this.inputs.values())
            s.sent = 0;
    }

    // Runs the cycle on the event loop: a timer to each turn's deadline.
    start(): void {
        if (this.timer !== undefined)
            return;
        this.due = this.clock();
        const tick = () => {
            this.poll();
            this.timer = setTimeout(tick, Math.max(0, this.due - this.clock()));
        };
        tick();
    }

    stop(): void {
        clearTimeout(this.timer);
        this.timer = undefined;
    }
}
