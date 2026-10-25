// The emulated system (spec §2, §7): the carrier's slots with their modules, every channel and
// signal with its state key (§8.3), the inputs' sources and noise, and the outputs' values. It
// computes a slot's inputs when asked (the scan cycle, plan task E5, decides when); time and
// randomness are injected.
import type {Catalog} from './catalog.js';
import {Checker, type Json} from './check.js';
import {
    hasNoise, isMeasured, type CarrierType, type CommParam, type CommType, type MeasuredInput, type ModuleType,
    type Output, type Signal, type SourcedInput,
} from './descriptors.js';
import {carrierOf, type Environment, type InputConfig} from './environment.js';
import {evaluate, evaluateNumber, fit, rollOver} from './evaluate.js';
import type {Random} from './random.js';
import {readSource, sourceKind, sourceToJson, type Source} from './sources.js';

// A refused change; its message is the control protocol's (§4.1).
export class ModelError extends Error {}

// Milliseconds of a monotonic clock.
export type Clock = () => number;

interface Item {
    slot: number;
    key: string;            // `slot1.ai.1` (control protocol §3.5)
    label: string;          // the hakkas port and channel, `slot1-ai[1]`
    port: string;           // `slot1-ai`
    index?: number;         // 1…count, absent for a group of one (04 §0: a plain port)
}

// A module input with a source of its own.
export interface SourcedChannel extends Item {
    kind: 'input';
    def: SourcedInput;
    source: Source;
    since: number;          // when the source was set (ms); a function's time starts there
    noise: number;
    value: number | boolean | null;
}

// A module input measured from a signal.
export interface MeasuredChannel extends Item {
    kind: 'measured';
    def: MeasuredInput;
    signal: SignalState;
    noise: number;
    value: number | boolean | null;
}

export interface OutputChannel extends Item {
    kind: 'output';
    def: Output;
    value: number | boolean;
}

// A field-side signal: a pulse train whose phase (its periods since power-up) runs on whatever
// the source does, so the count never jumps.
export interface SignalState extends Omit<Item, 'port'> {
    kind: 'signal';
    def: Signal;
    source: Extract<Source, {kind: 'pulse'}>;
    since: number;
    phase: number;          // periods since power-up
    at: number;             // when the phase was last advanced (ms)
    frequency: number;      // the frequency then (Hz)
    duty: number;           // the duty cycle then (%)
}

export type InputChannel = SourcedChannel | MeasuredChannel;
export type Channel = InputChannel | OutputChannel;

export interface Slot {
    index: number;
    part: string;           // `slot<n>` (spec §8.3)
    builtin: boolean;
    type?: string;          // the module type the environment puts here
    module?: ModuleType;    // undefined: empty, or a type the catalog lacks
    serial?: string;
    inputs: InputChannel[];
    outputs: OutputChannel[];
    signals: SignalState[];
}

export interface CommSlot {
    slot: number;
    def: CommType;
    params: Partial<Record<CommParam, Json>>;
}

export class System {
    readonly carrier: CarrierType;
    readonly slots: Slot[] = [];
    readonly comm: CommSlot[] = [];
    private readonly items = new Map<string, Channel | SignalState>();
    private readonly ports = new Map<string, Channel[]>();

    // `config` is the environment the system was built from; its inputs are what toEnvironment
    // computes again.
    private constructor(readonly name: string, readonly config: Environment, catalog: Catalog,
                        private readonly clock: Clock, private readonly random: Random) {
        const carrier = carrierOf(config, catalog);
        if (carrier === undefined)
            throw new Error(`unknown carrier type ${config.carrier as string}`);
        this.carrier = carrier;
        const now = clock();
        for (let n = 0; n < carrier.slots + carrier.builtin.length; n++) {
            const builtin = carrier.builtin.find(b => b.slot === n);
            const configured = builtin === undefined ? config.modules.find(m => m.slot === n) : undefined;
            const type = builtin?.type ?? configured?.type;
            const slot: Slot = {index: n, part: `slot${n}`, builtin: builtin !== undefined,
                inputs: [], outputs: [], signals: []};
            if (type !== undefined) {
                slot.type = type;
                const module = catalog.modules.get(type)?.def;
                if (module !== undefined)
                    slot.module = module;
            }
            if (configured?.serial !== undefined)
                slot.serial = configured.serial;
            if (slot.module !== undefined)
                this.addChannels(slot, slot.module, now);
            this.slots.push(slot);
        }
        for (const s of config.comm) {
            const def = catalog.comm.get(s.type)?.def;
            if (def === undefined || !carrier.comm.find(x => x.slot === s.slot)?.types.includes(s.type))
                continue;
            const params: CommSlot['params'] = {};
            for (const p of def.params)
                if (s[p] !== undefined)
                    params[p] = s[p];
            this.comm.push({slot: s.slot, def, params});
        }
    }

    // Builds the system of an environment that checkEnvironment found runnable. The inputs the
    // environment sets are applied; one that does not fit is left at its default, with a warning.
    static build(name: string, env: Environment, catalog: Catalog, clock: Clock,
                 random: Random): {system: System, warnings: string[]} {
        const system = new System(name, env, catalog, clock, random);
        const warnings: string[] = [];
        for (const [key, input] of Object.entries(env.inputs))
            system.applyInput(key, input, warnings);
        return {system, warnings};
    }

    private addChannels(slot: Slot, module: ModuleType, now: number): void {
        const at = (id: string, count: number, i: number) => {
            const index = count > 1 ? i : undefined;
            const suffix = index === undefined ? '' : `.${index}`;
            return {slot: slot.index, key: `${slot.part}.${id}${suffix}`, port: `${slot.part}-${id}`,
                label: `${slot.part}-${id}${index === undefined ? '' : `[${index}]`}`,
                ...(index !== undefined && {index})};
        };
        for (const def of module.signals)
            for (let i = 1; i <= def.count; i++) {
                const {port: _, ...item} = at(def.id, def.count, i);
                const source = def.default as SignalState['source'];
                const s: SignalState = {...item, kind: 'signal', def, source, since: now, phase: 0, at: now,
                    frequency: 0, duty: 0};
                [s.frequency, s.duty] = pulseNow(s, now);
                slot.signals.push(s);
                this.items.set(s.key, s);
            }
        for (const def of module.channels)
            for (let i = 1; i <= def.count; i++) {
                const item = at(def.id, def.count, i);
                let ch: Channel;
                if (def.direction === 'output')
                    ch = {...item, kind: 'output', def, value: def.default};
                else if (isMeasured(def)) {
                    const signal = slot.signals.find(s => s.def.id === def.measure.signal && s.index === item.index)!;
                    ch = {...item, kind: 'measured', def, signal, noise: def.noise, value: null};
                } else
                    ch = {...item, kind: 'input', def, source: def.default, since: now, noise: def.noise, value: null};
                if (ch.kind === 'output')
                    slot.outputs.push(ch);
                else
                    slot.inputs.push(ch);
                this.items.set(ch.key, ch);
                const list = this.ports.get(ch.port) ?? [];
                list.push(ch);
                this.ports.set(ch.port, list);
            }
    }

    // The channel of a hakkas port (04 §0.1 `value`/`write`): `index` for a group of several,
    // none for a group of one.
    channel(port: string, index?: number): Channel | undefined {
        return this.ports.get(port)?.find(ch => ch.index === index);
    }

    // The channels of a hakkas port, in index order; undefined for no such port.
    portChannels(port: string): readonly Channel[] | undefined {
        return this.ports.get(port);
    }

    item(key: string): Channel | SignalState | undefined {
        return this.items.get(key);
    }

    // Computes the slot's inputs at its turn (spec §4 step 2) and returns those whose value
    // changed (the first value always does).
    sample(slotIndex: number): InputChannel[] {
        const slot = this.slots[slotIndex];
        if (slot === undefined)
            throw new RangeError(`no slot ${slotIndex}`);
        const now = this.clock();
        for (const s of slot.signals)
            advance(s, now);
        const changed: InputChannel[] = [];
        for (const ch of slot.inputs) {
            const value = ch.kind === 'input' ? this.sourced(ch, now) : this.measured(ch);
            if (value !== ch.value) {
                ch.value = value;
                changed.push(ch);
            }
        }
        return changed;
    }

    private sourced(ch: SourcedChannel, now: number): number | boolean {
        const v = evaluate(ch.source, (now - ch.since) / 1000);
        if (typeof v === 'boolean')
            return v;
        if (ch.source.kind === 'counter')
            return rollOver(v, ch.def.min, ch.def.max);
        return fit(v + this.noise(ch.noise), ch.def);
    }

    private measured(ch: MeasuredChannel): number | boolean {
        const s = ch.signal;
        const steady = s.frequency === 0;
        switch (ch.def.measure.quantity) {
        case 'state':
            return steady ? s.duty >= 50 : s.phase % 1 < s.duty / 100;
        case 'frequency':
            return fit(s.frequency + this.noise(ch.noise), ch.def);
        case 'duty':
            return fit((steady ? (s.duty >= 50 ? 100 : 0) : s.duty) + this.noise(ch.noise), ch.def);
        case 'count':
            return rollOver(completedPeriods(s.phase), ch.def.min, ch.def.max);
        }
    }

    // A uniformly distributed value in ±noise, anew at each turn (spec §7.2).
    private noise(noise: number): number {
        return noise === 0 ? 0 : (2 * this.random() - 1) * noise;
    }

    // The value hakkas wrote, as the slot's turn applies it (spec §4 step 1).
    applyOutput(ch: OutputChannel, value: number | boolean): void {
        ch.value = value;
    }

    // Sets a `….source` or `….noise` key (control protocol §4.1); refusals are ModelErrors.
    set(key: string, value: Json): void {
        const attribute = key.endsWith('.source') ? 'source' : key.endsWith('.noise') ? 'noise' : undefined;
        const base = attribute === undefined ? key : key.slice(0, -attribute.length - 1);
        const item = this.items.get(base);
        if (item === undefined || attribute === undefined) {
            if (item !== undefined)
                throw new ModelError(`Not settable: ${key}`);
            throw new ModelError(`Unknown key: ${key}`);
        }
        if (attribute === 'source') {
            if (item.kind !== 'input' && item.kind !== 'signal')
                throw new ModelError(item.kind === 'output' ? `Not settable: ${key}` : `Unknown key: ${key}`);
            const accepted = item.kind === 'signal' ? ['pulse' as const] : item.def.sources;
            const kind = sourceKind(value);
            if (kind !== undefined && !accepted.includes(kind))
                throw new ModelError(`Source not accepted: ${key}`);
            const c = new Checker();
            const source = readSource(c, value, '', item.kind === 'signal' ? 'pulse' : item.def.type, accepted);
            if (source === undefined)
                throw new ModelError(`Invalid source: ${key}: ${c.errors.join('; ')}`);
            const now = this.clock();
            if (item.kind === 'signal') {
                advance(item, now);
                item.source = source as SignalState['source'];
                item.since = now;
                [item.frequency, item.duty] = pulseNow(item, now);
            } else {
                item.source = source;
                item.since = now;
            }
            return;
        }
        if (item.kind === 'signal' || item.kind === 'output' || !hasNoise(item.def))
            throw new ModelError(item.kind === 'output' ? `Not settable: ${key}` : `Unknown key: ${key}`);
        if (typeof value !== 'number' || !Number.isFinite(value) || value < 0)
            throw new ModelError(`Invalid noise: ${key}`);
        item.noise = value;
    }

    private applyInput(key: string, input: InputConfig, warnings: string[]): void {
        const item = this.items.get(key);
        if (item === undefined || (item.kind !== 'input' && item.kind !== 'signal' && item.kind !== 'measured')) {
            warnings.push(`inputs.${key}: no such input or signal, dropped`);
            return;
        }
        for (const attribute of ['source', 'noise'] as const)
            if (input[attribute] !== undefined)
                try {
                    this.set(`${key}.${attribute}`, input[attribute]!);
                } catch (e) {
                    warnings.push(`inputs.${key}.${attribute}: ${(e as Error).message}, the default kept`);
                }
    }

    // Every state key with its value (control protocol §3.5): channel values, and the inputs'
    // and signals' sources and noise.
    state(): Record<string, Json> {
        const out: Record<string, Json> = {};
        for (const item of this.items.values()) {
            if (item.kind !== 'signal')
                out[item.key] = item.value;
            if (item.kind === 'input' || item.kind === 'signal')
                out[`${item.key}.source`] = sourceToJson(item.source);
            if ((item.kind === 'input' || item.kind === 'measured') && hasNoise(item.def))
                out[`${item.key}.noise`] = item.noise;
        }
        return out;
    }

    // The environment to persist (spec §5.1): the configuration it was built from, with the
    // inputs and signals whose source or noise differ from their module's defaults.
    toEnvironment(): Environment {
        const inputs: Record<string, InputConfig> = {};
        for (const item of this.items.values()) {
            const entry: InputConfig = {};
            if (item.kind === 'input' || item.kind === 'signal') {
                const source = sourceToJson(item.source);
                if (JSON.stringify(source) !== JSON.stringify(sourceToJson(item.def.default)))
                    entry.source = source;
            }
            if ((item.kind === 'input' || item.kind === 'measured') && item.noise !== item.def.noise)
                entry.noise = item.noise;
            if (Object.keys(entry).length > 0)
                inputs[item.key] = entry;
        }
        return {...structuredClone(this.config), inputs};
    }
}

// The periods a phase has completed. The phase is a sum of many small steps, so a whole number
// of periods may come out a hair below it (499.9999999 for 500); that still counts.
function completedPeriods(phase: number): number {
    return Math.floor(phase + 1e-9);
}

// A pulse train's frequency (≥ 0) and duty cycle (0-100 %) at `now`.
function pulseNow(s: SignalState, now: number): [number, number] {
    const t = (now - s.since) / 1000;
    const frequency = Math.max(0, evaluateNumber(s.source.frequency, t));
    const duty = Math.min(Math.max(evaluateNumber(s.source.duty, t), 0), 100);
    return [frequency, duty];
}

// Moves a signal's phase on to `now`: the frequency integrated over the time since it was last
// advanced (by the trapezoid rule, exact for a constant frequency).
function advance(s: SignalState, now: number): void {
    const [frequency, duty] = pulseNow(s, now);
    s.phase += (s.frequency + frequency) / 2 * (now - s.at) / 1000;
    s.at = now;
    s.frequency = frequency;
    s.duty = duty;
}
