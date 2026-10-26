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

// Where a channel is on its module's picture (plan task 75): fractions (0..1) of the picture's
// width and height from its top left corner; a built-in module's, without a picture, of the
// carrier's. Not checked by hakkas: one naming no channel, or outside 0..1, is to be skipped.
export interface EnvPosition {
    port: string;
    index?: number;         // the channel of an addressable port
    x: number;
    y: number;
}

// Where a slot is on the carrier's picture: a rectangle in fractions of its width and height.
export interface EnvRect {
    x: number;
    y: number;
    w: number;
    h: number;
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
    picture?: string;       // a link; only an https one is to be loaded
    ports: string[];
    positions?: EnvPosition[];
}

export interface EnvComm {
    slot: number;
    type?: string;
    name?: string;
    picture?: string;
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
    picture?: string;
}

export interface EnvCarrier {
    type?: string;
    name?: string;
    vendor?: string;
    revision?: string;
    serial?: string;
    documentation?: string;
    picture?: string;
    slots: number;
    builtin: { slot: number }[];
    power?: { voltage?: { min?: number, max?: number }, ac?: boolean, dc?: boolean, current?: number };
    comm: EnvComm[];
    mcu?: EnvMcu;
    places?: { slots?: (EnvRect & { slot: number })[], comm?: (EnvRect & { slot: number })[], mcu?: EnvRect };
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
