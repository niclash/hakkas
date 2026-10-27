import {NetworkProtocol} from "@/components/protocols/network";
import {ComponentProtocol} from "@/components/protocols/component";
import {GraphProtocol} from "@/components/protocols/graph";
import {TraceProtocol} from "@/components/protocols/trace";
import {RuntimeProtocol} from "@/components/protocols/runtime";
import {Connection} from "@/components/protocols/websocket";
import {AuthProtocol} from "@/components/protocols/auth";
import {EnvironmentProtocol} from "@/components/protocols/environment";
import {LinkProtocol} from "@/components/protocols/link";

export interface Port {
    id: string;
    type: string;
    schema: string;
    required: boolean;
    addressable: boolean;
    description: string;
    values: string[];
    default: any;
}


export class Protocols {
    network: NetworkProtocol;
    component: ComponentProtocol;
    graph: GraphProtocol;
    runtime: RuntimeProtocol;
    trace: TraceProtocol;
    auth: AuthProtocol;
    environment: EnvironmentProtocol;
    link: LinkProtocol;

    constructor(connection: Connection) {
        this.network = new NetworkProtocol(connection);
        this.component = new ComponentProtocol(connection);
        this.graph = new GraphProtocol(connection);
        this.runtime = new RuntimeProtocol(connection);
        this.trace = new TraceProtocol(connection);
        this.auth = new AuthProtocol(connection);
        this.environment = new EnvironmentProtocol(connection);
        this.link = new LinkProtocol(connection);
    }

    onMessage(protocol: string, command: string, payload: any) {
        switch (protocol) {
            case "component":
                this.component.execute(command, payload);
                break;
            case "graph":
                this.graph.execute(command, payload);
                break;
            case "network":
                this.network.execute(command, payload);
                break;
            case "runtime":
                this.runtime.execute(command, payload);
                break;
            case "trace":
                this.trace.execute(command, payload);
                break;
            case "auth":
                this.auth.execute(command, payload);
                break;
            case "environment":
                this.environment.execute(command, payload);
                break;
            case "link":
                this.link.execute(command, payload);
                break;
        }
        /*
                    TODO: Check authorization in what way?
                    TODO: Invalid requests causes an exception!
        */

    }
}
