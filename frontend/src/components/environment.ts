// The engine's environment, as its messages show it (02 §3.7, 06 §4.14): the description, and
// every process's bindings. Changed only by the engine's events (02 §1.1).

import {reactive} from "vue";
import type {Binding, EnvDescription} from "@/components/protocols/environment";
import {Connection} from "@/components/protocols/websocket";
import {errorClaims} from "@/components/model";
import {channelLabel, isSensor} from "@/components/bindings";
import {attachDevice, device, watchDevice} from "@/components/device";

export const environment = reactive({
    // The description; undefined until it came, or when there is none (`none`).
    description: undefined as EnvDescription | undefined,
    none: false,
    bindings: [] as Binding[],
});

const same = (a: Binding, b: Binding) =>
    a.graph === b.graph && a.port === b.port && a.env === b.env && (a.index ?? 0) === (b.index ?? 0);

// Whether a binding is of an inport (a sensor's) or an outport (an actuator's), by its
// environment port (04 §4); undefined for a port the description does not have.
export function directionOf(b: Binding): "in" | "out" | undefined {
    const d = environment.description;
    const sensor = d === undefined ? undefined : isSensor(d, b.env);
    return sensor === undefined ? undefined : sensor ? "in" : "out";
}

let describing = false;
let waiting: (() => void)[] = [];

// Feeds the store from a connection's events; called once per connection, before login.
export function attachEnvironment(conn: Connection) {
    environment.description = undefined;
    environment.none = false;
    environment.bindings = [];
    waiting = [];
    const env = conn.proto.environment;
    env.addListener("onDescription", (d: EnvDescription) => {
        describing = false;
        environment.description = d;
        environment.none = false;
    });
    env.addListener("onBindings", (p: { bindings: Binding[] }) => {
        environment.bindings = p.bindings ?? [];
        const done = waiting;
        waiting = [];
        done.forEach(fn => fn());
    });
    env.addListener("onBind", (b: Binding) => {
        if (!environment.bindings.some(x => same(x, b)))
            environment.bindings.push(b);
    });
    env.addListener("onUnbind", (b: Binding) => {
        environment.bindings = environment.bindings.filter(x => !same(x, b));
    });
    // A renamed process port keeps its bindings (04 §4, plan task 70): no binding event says so.
    const renamed = (direction: "in" | "out") => (p: { graph: string, from: string, to: string }) => {
        for (const b of environment.bindings)
            if (b.graph === p.graph && b.port === p.from && directionOf(b) === direction)
                b.port = p.to;
    };
    conn.proto.graph.addListener("onGraphRenameInport", renamed("in"));
    conn.proto.graph.addListener("onGraphRenameOutport", renamed("out"));
    // Bindings of a process this connection does not view come with no event (02 §4.1): read
    // them all again whenever a process is opened.
    conn.proto.graph.addListener("onGraphClear", () => conn.proto.environment.request_getbindings());
    attachDevice(conn);
}

// Asked once after login; an engine without one says "No environment".
export function describeEnvironment(conn: Connection) {
    describing = true;
    conn.proto.environment.request_describe();
    // An open device window watches again after a reconnect (06 §4.15).
    if (device.open)
        watchDevice(conn, true);
}

errorClaims.push((message: string) => {
    if (!describing || message !== "No environment")
        return false;
    describing = false;
    environment.none = true;
    return true;
});

// Every process's bindings, read again; resolves when they are in.
export function refreshBindings(conn: Connection): Promise<void> {
    return new Promise(resolve => {
        waiting.push(resolve);
        conn.proto.environment.request_getbindings();
    });
}

export function bindingsOf(graph: string): Binding[] {
    return environment.bindings.filter(b => b.graph === graph);
}

// The bindings of a process port: an inport has at most one (a sensor), an outport may drive
// several actuators.
export function bindingsAt(graph: string, direction: "in" | "out", port: string): Binding[] {
    return environment.bindings.filter(b => b.graph === graph && b.port === port && directionOf(b) === direction);
}

// What a channel is bound to.
export function boundTo(env: string, index?: number): Binding[] {
    return environment.bindings.filter(b => b.env === env && (b.index ?? 0) === (index ?? 0));
}

export function bindingLabel(b: Binding): string {
    return channelLabel(b.env, b.index);
}
