import {ClassicPreset as Classic, GetSchemes} from "rete";
import {ComponentEvent} from "@/components/protocols/component";
import {Port} from "@/components/protocols/protocols";
import {Ref, UnwrapRef} from "@vue/reactivity";
import {ref, shallowRef, ShallowRef} from "vue";
import {Connection as WsConnection} from "@/components/protocols/websocket";
import {ClearEvent} from "@/components/protocols/graph";
import {NetworkErrorEvent, StartedEvent, StatusEvent, StoppedEvent} from "@/components/protocols/network";

const socket = new Classic.Socket('socket');

export let componentTemplates: Record<string, ComponentEvent> = {};

export type Node = PrimitiveNode;
type Conn =
    | Connection<PrimitiveNode, PrimitiveNode>
    ;

export class Connection<A extends Node, B extends Node> extends Classic.Connection<A, B> {
}

export type Schemes = GetSchemes<Node, Conn>;

export class Graph {
    constructor(id: string, name: string, description: string, debug: boolean) {
        this.graph = id;
        this.name = name;
        this.description = description;
        this.debug = debug;
        this.running = false;
        this.started = false;
        this.uptime = 0;
    }

    name: string;
    description: string;
    graph: string;
    debug: boolean;
    running: boolean;
    started: boolean;
    uptime: number;
}

export class PrimitiveNode extends Classic.Node {
    readonly component: string;
    readonly inports: Port[];
    readonly outports: Port[];
    description: string;
    width = 180;
    height = 195;

    constructor(name: string, description: string, component: string, inports: Port[], outports: Port[]) {
        super(name);
        this.component = component;
        this.description = description;
        this.inports = inports;
        this.outports = outports;
        let inHeight = inports.length * 36;
        let outHeight = outports.length * 36;
        this.height = 70 + inHeight + outHeight;
        for (let outport of outports) {
            this.addOutput(outport.id, new Classic.Output(socket, outport.id));
        }
        for (let inport of inports) {
            this.addInput(inport.id, new Classic.Input(socket, inport.id ));
        }
    }

    // Shows an input's initial (a literal wired to it) in its label; undefined removes it.
    setInitial(port: string, data: string | undefined) {
        let input = this.inputs[port];
        if (input !== undefined)
            input.label = data === undefined ? port : port + " = " + data;
    }
}

export const vueModel: {
    graphs: Ref<UnwrapRef<Map<string, Graph>>>,
    currentGraph: ClearEvent,
    currentGraphId: Ref<string>,
    selectedNodes: Node[],
    pipesDisabled: boolean,
    components: Ref<UnwrapRef<ComponentEvent[]>>,
    selectedComponent: string,
    connection: ShallowRef<WsConnection | undefined>,
    errors: Ref<string[]>,
    counters: Record<string, number>,
    nextName: () => string,
} = {
    graphs: ref(new Map<string, Graph>()),
    currentGraph: {id: "", name: "", icon: "", description: "", library: "", main: false},
    currentGraphId: ref(""),
    components: ref([]),
    pipesDisabled: false,
    selectedNodes: [],
    connection: shallowRef(undefined),
    errors: ref([]),
    selectedComponent: "",
    counters: {},
    nextName: () => {
        let count = vueModel.counters[vueModel.selectedComponent];
        if (count === undefined)
            count = 0;
        else
            count++;
        vueModel.counters[vueModel.selectedComponent] = count;
        return vueModel.selectedComponent + count;
    }
};

export function showError(message: string) {
    vueModel.errors.value.push(message);
    if (vueModel.errors.value.length > 5)
        vueModel.errors.value.shift();
}

// Feeds the model from a connection's events; the model is never changed by a request
// (specifications/02-wire-protocol.md §1.1). Called once per connection, before login.
export function attachModel(conn: WsConnection) {
    vueModel.graphs.value = new Map<string, Graph>();
    vueModel.components.value = [];
    componentTemplates = {};
    vueModel.currentGraph = {id: "", name: "", icon: "", description: "", library: "", main: false};
    vueModel.currentGraphId.value = "";

    const graph = (id: string): Graph => {
        let g = vueModel.graphs.value.get(id);
        if (g === undefined) {
            g = new Graph(id, id, "", false);
            vueModel.graphs.value.set(id, g);
        }
        return vueModel.graphs.value.get(id)!;   // the reactive proxy
    };

    const proto = conn.proto;
    proto.component.addListener("onComponentComponent", (c: ComponentEvent) => {
        componentTemplates[c.name] = c;
        const list = vueModel.components.value;
        const i = list.findIndex(existing => existing.name === c.name);
        if (i >= 0)
            list[i] = c;
        else
            list.push(c);
    });
    proto.network.addListener("onNetworkStatus", (s: StatusEvent) => {
        Object.assign(graph(s.graph), {
            name: s.name, description: s.description, running: s.running, started: s.started, uptime: s.uptime
        });
    });
    proto.network.addListener("onNetworkStarted", (s: StartedEvent) => {
        Object.assign(graph(s.graph), {running: true, started: true});
    });
    proto.network.addListener("onNetworkStopped", (s: StoppedEvent) => {
        Object.assign(graph(s.graph), {running: false, started: false, uptime: 0});
    });
    proto.network.addListener("onNetworkError", (e: NetworkErrorEvent) => showError(e.message));
    proto.runtime.addListener("onNewGraphPacket", (p: { graph: string, name: string, description: string }) => {
        Object.assign(graph(p.graph), {name: p.name, description: p.description});
    });
    proto.runtime.addListener("onDeleteGraphPacket", (p: { graph: string }) => {
        vueModel.graphs.value.delete(p.graph);
    });
    proto.graph.addListener("onGraphClear", (p: ClearEvent) => {
        vueModel.currentGraphId.value = p.id;
        Object.assign(graph(p.id), {name: p.name, description: p.description});
    });
    proto.graph.addListener("onGraphRename", (p: { id: string, to: string }) => {
        graph(p.id).name = p.to;
    });
}
