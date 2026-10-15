import {ClassicPreset as Classic, GetSchemes} from "rete";
import {ComponentEvent} from "@/components/protocols/component";
import {Port} from "@/components/protocols/protocols";
import {Ref, UnwrapRef} from "@vue/reactivity";
import {computed, reactive, ref, shallowRef, ShallowRef} from "vue";
import {errorText, MessageLog, parseLogLine} from "@/components/messages";
import {permissions} from "@/components/roles";
import {Connection as WsConnection} from "@/components/protocols/websocket";
import {ClearEvent} from "@/components/protocols/graph";
import {SelectedBlock} from "@/components/inspector";
import {NetworkErrorEvent, StartedEvent, StatusEvent, StoppedEvent} from "@/components/protocols/network";

// One socket per port type, named after it (06 §4.5): its color and tooltip show the type.
const sockets = new Map<string, Classic.Socket>();
export function socketFor(portType: string): Classic.Socket {
    let socket = sockets.get(portType);
    if (socket === undefined) {
        socket = new Classic.Socket(portType);
        sockets.set(portType, socket);
    }
    return socket;
}

export let componentTemplates: Record<string, ComponentEvent> = {};

export type Node = PrimitiveNode | ValueNode | PortNode;
type Conn =
    | Connection<PrimitiveNode, PrimitiveNode>
    | Connection<ValueNode, PrimitiveNode>
    | Connection<PortNode, PrimitiveNode>
    | Connection<PrimitiveNode, PortNode>
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
        this.uptimeAt = Date.now();
        this.unsaved = false;
    }

    name: string;
    description: string;
    graph: string;
    debug: boolean;
    running: boolean;
    started: boolean;
    uptime: number;     // seconds, at uptimeAt (ms)
    uptimeAt: number;
    unsaved: boolean;   // differs from its graph file (02 §3.3, "Unsaved state")
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
            this.addOutput(outport.id, new Classic.Output(socketFor(outport.type ?? ""), outport.id));
        }
        for (let inport of inports) {
            this.addInput(inport.id, new Classic.Input(socketFor(inport.type ?? ""), inport.id));
        }
    }

    // The inputs' initials (literals wired to them), as the events carry them; each is drawn as
    // a ValueNode connected to its input.
    readonly initials: Record<string, string> = {};

    setInitial(port: string, data: string | undefined) {
        if (data === undefined)
            delete this.initials[port];
        else
            this.initials[port] = data;
    }

    // The type of an input's port ("" when unknown).
    inputType(port: string): string {
        return this.inports.find(p => p.id === port)?.type ?? "";
    }
}

// A fixed value (an initial, 02 §3.4) drawn as a small node connected to its input (06 §4.3).
// It is not a block of the process: it has no id there, cannot be selected or dragged, and is
// placed beside its input (it stores no position, as the graph file's connection from a value
// does not).
export class ValueNode extends Classic.Node {
    readonly block: string;     // the editor's id of the block whose input it feeds
    readonly port: string;
    readonly portType: string;
    value: string;              // as the event carries it
    blockLabel: string;         // the block's id in the process, for commands
    editRequested = false;      // its menu's "Change value…": open the field at the next update
    width = 165;
    height = 36;

    constructor(block: PrimitiveNode, port: string, value: string) {
        super("");
        this.block = block.id;
        this.blockLabel = block.label;
        this.port = port;
        this.portType = block.inputType(port);
        this.value = value;
        this.addOutput("value", new Classic.Output(socketFor(this.portType), ""));
    }
}

export function isValueNode(node: Node | undefined): node is ValueNode {
    return node instanceof ValueNode;
}

// A process port (02 §3.4 addinport/addoutport, 06 §4.8): a marker at the process's edge,
// connected to the block port it exposes. Like a value node it is not a block: not selected,
// not dragged, placed by the editor.
export class PortNode extends Classic.Node {
    readonly direction: "in" | "out";
    name: string;               // the public name
    readonly block: string;     // the editor's id of the block
    readonly port: string;
    width = 260;
    height = 36;

    constructor(direction: "in" | "out", name: string, block: PrimitiveNode, port: string) {
        super(name);
        this.direction = direction;
        this.name = name;
        this.block = block.id;
        this.port = port;
        const type = direction === "in" ? block.inputType(port) : block.outports.find(p => p.id === port)?.type ?? "";
        if (direction === "in")
            this.addOutput("port", new Classic.Output(socketFor(type), ""));
        else
            this.addInput("port", new Classic.Input(socketFor(type), ""));
    }
}

export function isPortNode(node: Node | undefined): node is PortNode {
    return node instanceof PortNode;
}

export const vueModel: {
    graphs: Ref<UnwrapRef<Map<string, Graph>>>,
    currentGraph: ClearEvent,
    currentGraphId: Ref<string>,
    pipesDisabled: boolean,
    // The selected blocks of the open process (06 §4.0), for the inspector.
    selectedBlocks: Ref<SelectedBlock[]>,
    // Incremented to ask the inspector to open its name field (F2, the context menu).
    renameRequest: Ref<number>,
    // Incremented to ask the inspector to focus its first input's value (the context menu).
    inputsRequest: Ref<number>,
    // The open graph when it is a component graph, a block type's definition (02 §3.4.3).
    currentComponent: Ref<ClearEvent | undefined>,
    // The session's capabilities, from its login (02 §2.4), and who it is.
    capabilities: Ref<string[]>,
    user: Ref<string>,
    role: Ref<string>,
    // The engine's runtime:runtime (its label, version, id), for the About view.
    runtime: Ref<Record<string, unknown> | undefined>,
    components: Ref<UnwrapRef<ComponentEvent[]>>,
    selectedComponent: string,
    connection: ShallowRef<WsConnection | undefined>,
    errors: Ref<string[]>,
    nextName: (component: string, exists: (name: string) => boolean) => string,
} = {
    graphs: ref(new Map<string, Graph>()),
    currentGraph: {id: "", name: "", icon: "", description: "", library: "", main: false},
    currentGraphId: ref(""),
    components: ref([]),
    pipesDisabled: false,
    selectedBlocks: ref([]),
    renameRequest: ref(0),
    inputsRequest: ref(0),
    currentComponent: ref(undefined),
    capabilities: ref([]),
    user: ref(""),
    role: ref(""),
    runtime: ref(undefined),
    connection: shallowRef(undefined),
    errors: ref([]),
    selectedComponent: "",
    // A new block's id: its type's name and the first number not taken in the process
    // (math/Add2 -> Add2_1, Add2_2, ...).
    nextName: (component: string, exists: (name: string) => boolean) => {
        const base = component.substring(component.lastIndexOf('/') + 1);
        for (let n = 1; ; n++) {
            if (!exists(base + "_" + n))
                return base + "_" + n;
        }
    }
};

// Parts of the UI that show some refusals themselves, at the field the command came from:
// each one tells whether it took the message.
export const errorClaims: ((message: string, graph?: string) => boolean)[] = [];

// The messages panel's log (06 §4.12): kept across connections.
export const messageLog = reactive(new MessageLog());

// What the session may do (06 §4.11): the UI offers only that.
export const can = computed(() => permissions(vueModel.capabilities.value));

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
    vueModel.selectedBlocks.value = [];
    vueModel.currentComponent.value = undefined;
    vueModel.capabilities.value = [];

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
            name: s.name, description: s.description, running: s.running, started: s.started, uptime: s.uptime,
            uptimeAt: Date.now(), unsaved: s.unsaved ?? false
        });
    });
    proto.network.addListener("onNetworkStarted", (s: StartedEvent) => {
        Object.assign(graph(s.graph), {running: true, started: true, uptime: 0, uptimeAt: Date.now()});
    });
    proto.network.addListener("onNetworkStopped", (s: StoppedEvent) => {
        Object.assign(graph(s.graph), {running: false, started: false, uptime: s.uptime ?? 0, uptimeAt: Date.now()});
    });
    proto.network.addListener("onNetworkError", (e: NetworkErrorEvent) => {
        // A mirrored error line of the engine's log (05 §4): the messages panel only.
        const line = parseLogLine(e.message);
        if (line !== undefined) {
            messageLog.add({level: line.level, text: line.text, source: line.source});
            return;
        }
        const name = e.graph ? vueModel.graphs.value.get(e.graph)?.name : undefined;
        const text = errorText(e.message, name, e.graph === vueModel.currentGraphId.value);
        messageLog.add({level: "error", text, refused: true});
        if (!errorClaims.some(claim => claim(e.message, e.graph)))
            showError(text);
    });
    proto.network.addListener("onNetworkOutput", (o: { message: string }) => {
        const line = parseLogLine(o.message ?? "");
        messageLog.add(line !== undefined ? {level: line.level, text: line.text, source: line.source}
            : {level: "info", text: String(o.message ?? "")});
    });
    proto.runtime.addListener("onRuntime", (r: Record<string, unknown>) => {
        vueModel.runtime.value = r;
    });
    proto.runtime.addListener("onNewGraphPacket", (p: { graph: string, name: string, description: string }) => {
        Object.assign(graph(p.graph), {name: p.name, description: p.description});
    });
    proto.runtime.addListener("onDeleteGraphPacket", (p: { graph: string }) => {
        vueModel.graphs.value.delete(p.graph);
        // A component graph's deletion removes its type (02 §3.4.3): no component:* says so.
        const list = vueModel.components.value;
        const type = list.findIndex(c => c.name === p.graph && c.subgraph);
        if (type >= 0) {
            list.splice(type, 1);
            delete componentTemplates[p.graph];
        }
        if (vueModel.currentComponent.value?.id === p.graph)
            vueModel.currentComponent.value = undefined;
        // The open process is gone: nothing is open (the editor clears).
        if (vueModel.currentGraphId.value === p.graph) {
            vueModel.currentGraphId.value = "";
            vueModel.currentGraph = {id: "", name: "", icon: "", description: "", library: "", main: false};
        }
    });
    proto.graph.addListener("onGraphClear", (p: ClearEvent) => {
        vueModel.currentGraphId.value = p.id;
        // A component graph is no process: it is not in the process list.
        vueModel.currentComponent.value = p.main === false ? p : undefined;
        if (p.main !== false)
            Object.assign(graph(p.id), {name: p.name, description: p.description});
    });
    proto.graph.addListener("onGraphRename", (p: { id: string, to: string }) => {
        const shown = vueModel.currentComponent.value;
        if (shown?.id === p.id)
            vueModel.currentComponent.value = {...shown, name: p.to};
        else if (vueModel.graphs.value.has(p.id))
            graph(p.id).name = p.to;
    });
}
