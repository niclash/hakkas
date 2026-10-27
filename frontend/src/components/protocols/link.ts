import {Connection} from "@/components/protocols/websocket";

// One end of a link between processes (02 §3.8): a process and its port's public name.
export interface LinkEnd {
    graph: string;
    port: string;
}

// A link: a process's outport (src) to a process's inport (tgt).
export interface ProcessLink {
    src: LinkEnd;
    tgt: LinkEnd;
}

// A process's ports, as link:links lists them: their public names in creation order.
export interface ProcessPorts {
    graph: string;
    inports: string[];
    outports: string[];
}

// The link group of specifications/02-wire-protocol.md §3.8 (plan task 79): every link and
// every process's ports, and making and removing a link.
export class LinkProtocol {
    private connection: Connection;
    listeners: {
        onLinks: Function[],    // {links: ProcessLink[], processes: ProcessPorts[]}
        onAdd: Function[],      // ProcessLink
        onRemove: Function[],   // ProcessLink
    };

    constructor(connection: Connection) {
        this.connection = connection;
        this.listeners = {onLinks: [], onAdd: [], onRemove: []};
    }

    addListener(method: keyof LinkProtocol['listeners'], listener: Function): void {
        this.listeners[method].push(listener);
    }

    execute(command: string, payload: any) {
        const fire = (fns: Function[]) => fns.forEach(fn => fn(payload));
        switch (command) {
            case "links": fire(this.listeners.onLinks); break;
            case "add": fire(this.listeners.onAdd); break;
            case "remove": fire(this.listeners.onRemove); break;
        }
    }

    request_list() {
        this.connection.send({protocol: "link", command: "list", payload: {}});
    }

    request_add(l: ProcessLink) {
        this.connection.send({protocol: "link", command: "add", payload: {src: {...l.src}, tgt: {...l.tgt}}});
    }

    request_remove(l: ProcessLink) {
        this.connection.send({protocol: "link", command: "remove", payload: {src: {...l.src}, tgt: {...l.tgt}}});
    }
}
