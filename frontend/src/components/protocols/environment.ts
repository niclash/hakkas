import {Connection} from "@/components/protocols/websocket";

// The environment's description (specifications/04-drivers.md §1), as environment:description
// carries it.
export interface EnvPort {
    id: string;
    type: "number" | "bool";
    description?: string;
    unit?: string;
    min?: number;
    max?: number;
    addressable?: boolean;
    count?: number;
}

export interface EnvModule {
    slot?: number;
    builtin?: boolean;
    type?: string;
    name?: string;          // human-readable, e.g. "Colibri AIV" for colibri_aiv
    vendor?: string;
    revision?: string;
    serial?: string;
    documentation?: string;
    ports: string[];
}

export interface EnvComm {
    slot: number;
    type?: string;
    name?: string;
    ip?: { mode?: string, address?: string, prefix?: number, gateway?: string, dns?: string[], mac?: string };
    wifi?: { ssid?: string };
    rs485?: { bitrate?: number, parity?: string };
    lorawan?: { appEui?: string, devEui?: string, appKeySet?: boolean };
}

// The carrier's MCU module (plan task 73).
export interface EnvMcu {
    type?: string;
    name?: string;          // human-readable, e.g. "Colibri MCU6" for colibri_mcu6
    vendor?: string;
    revision?: string;
    serial?: string;
    documentation?: string;
}

export interface EnvCarrier {
    type?: string;
    name?: string;
    vendor?: string;
    revision?: string;
    serial?: string;
    documentation?: string;
    slots: number;
    builtin: { slot: number }[];
    power?: { voltage?: { min?: number, max?: number }, ac?: boolean, dc?: boolean, current?: number };
    comm: EnvComm[];
    mcu?: EnvMcu;
}

export interface EnvDescription {
    name: string;
    description?: string;
    icon?: string;
    outPorts: EnvPort[];    // sensors
    inPorts: EnvPort[];     // actuators
    modules: EnvModule[];
    carrier?: EnvCarrier;
}

// A binding of a process port to an environment channel (02 §3.7, plan task 70): a bindings
// file's entry plus its process. `port` is the process's inport of that name for a sensor, its
// outport for an actuator (the environment port decides, 04 §4).
export interface Binding {
    graph: string;
    port: string;
    env: string;
    index?: number;
}

// The environment group of specifications/02-wire-protocol.md §3.7: the description, the
// bindings, and binding and unbinding a process port.
export class EnvironmentProtocol {
    private connection: Connection;
    listeners: {
        onDescription: Function[],
        onBindings: Function[],
        onBind: Function[],
        onUnbind: Function[],
    };

    constructor(connection: Connection) {
        this.connection = connection;
        this.listeners = {onDescription: [], onBindings: [], onBind: [], onUnbind: []};
    }

    addListener(method: keyof EnvironmentProtocol['listeners'], listener: Function): void {
        this.listeners[method].push(listener);
    }

    execute(command: string, payload: any) {
        const fire = (fns: Function[]) => fns.forEach(fn => fn(payload));
        switch (command) {
            case "description": fire(this.listeners.onDescription); break;
            case "bindings": fire(this.listeners.onBindings); break;
            case "bind": fire(this.listeners.onBind); break;
            case "unbind": fire(this.listeners.onUnbind); break;
        }
    }

    request_describe() {
        this.connection.send({protocol: "environment", command: "describe", payload: {}});
    }

    request_getbindings() {
        this.connection.send({protocol: "environment", command: "getbindings", payload: {}});
    }

    request_bind(b: Binding) {
        this.request_raw("bind", b);
    }

    request_unbind(b: Binding) {
        this.request_raw("unbind", b);
    }

    // bind or unbind with its payload as it is (the undo history's commands, 06 §4.13).
    request_raw(command: string, payload: object) {
        this.connection.send({protocol: "environment", command, payload});
    }
}
