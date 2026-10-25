// The engine's environment, as its messages show it (02 §3.7, 06 §4.14): the description, and
// every process's bindings. Changed only by the engine's events (02 §1.1).

import {reactive} from "vue";
import type {Binding, EnvDescription} from "@/components/protocols/environment";
import {Connection} from "@/components/protocols/websocket";
import {errorClaims} from "@/components/model";
import {channelLabel} from "@/components/bindings";

export const environment = reactive({
    // The description; undefined until it came, or when there is none (`none`).
    description: undefined as EnvDescription | undefined,
    none: false,
    bindings: [] as Binding[],
});

const same = (a: Binding, b: Binding) =>
    a.graph === b.graph && a.block === b.block && a.port === b.port && a.env === b.env && (a.index ?? 0) === (b.index ?? 0);

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
    // A renamed block keeps its bindings (04 §4): no binding event says so.
    conn.proto.graph.addListener("onGraphRenameNode", (p: { graph: string, from: string, to: string }) => {
        for (const b of environment.bindings)
            if (b.graph === p.graph && b.block === p.from)
                b.block = p.to;
    });
    // Bindings of a process this connection does not view come with no event (02 §4.1): read
    // them all again whenever a process is opened.
    conn.proto.graph.addListener("onGraphClear", () => conn.proto.environment.request_getbindings());
}

// Asked once after login; an engine without one says "No environment".
export function describeEnvironment(conn: Connection) {
    describing = true;
    conn.proto.environment.request_describe();
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

export function bindingAt(graph: string, block: string, port: string): Binding | undefined {
    return environment.bindings.find(b => b.graph === graph && b.block === block && b.port === port);
}

// What a channel is bound to.
export function boundTo(env: string, index?: number): Binding[] {
    return environment.bindings.filter(b => b.env === env && (b.index ?? 0) === (index ?? 0));
}

export function bindingLabel(b: Binding): string {
    return channelLabel(b.env, b.index);
}
