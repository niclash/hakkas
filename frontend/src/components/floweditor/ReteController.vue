<script setup lang="ts">

import {computed, onMounted, ref, watch} from "vue";
import {createEditor} from './viewmodel';
import {editor} from './viewmodel/default';
import {componentTemplates, Connection, isValueNode, PrimitiveNode, ValueNode, vueModel} from "../model";
import {SocketData} from "rete-connection-plugin";
import {inputSources, placeValues, renameValues, showValue} from "./values";
import {
  attachLive, blockRemoved, blockRenamed, connectionLost, connectionsChanged, dataArrived, inputsOf, isWatched,
  live, processOpened, unwatch, watch as watchInputs, watchAll
} from "./live";
import ContextMenu from "./ContextMenu.vue";
import {MenuAction, menuFor, MenuItem, MenuTarget} from "./menus";
import {Point} from "@/components/watches";
import ValueDraft from "./ValueDraft.vue";
import {BLOCK_TYPE_MIME} from "@/components/palette";
import {
  AddEdgeEvent,
  AddGroupEvent,
  AddInitialEvent,
  AddInportEvent,
  AddNodeEvent,
  AddOutportEvent,
  ChangeEdgeEvent,
  ChangeGroupEvent,
  ChangeNodeEvent,
  ClearEvent,
  GraphProtocol,
  RemoveEdgeEvent,
  RemoveGroupEvent,
  RemoveInitialEvent,
  RemoveInportEvent,
  RemoveNodeEvent,
  RemoveOutportEvent,
  RenameGroupEvent,
  RenameInportEvent,
  RenameNodeEvent,
  RenameOutportEvent
} from "@/components/protocols/graph";
import {Port} from "@/components/protocols/protocols";
import {Position} from "rete-area-plugin/_types/types";
import {AreaExtensions} from "rete-area-plugin";

const rete = ref<HTMLElement | null>(null);

const startTransaction = (payload: any, skipGraphIdCheck: boolean = false) => {
  if (!skipGraphIdCheck && vueModel.currentGraph.id !== payload.graph)   // try to ensure we don't get superfluous messages from old interactions, or from other users.
    throw 'invalid graph id';
  console.log("Transaction { ");
  vueModel.pipesDisabled = true;
}

const endTransaction = () => {
  vueModel.pipesDisabled = false;
  console.log("};");
};

// Graph events are applied one at a time, in arrival order: each handler awaits the editor,
// and an event must not see the editor half way through the previous one (e.g. an addedge
// before its nodes exist).
let eventQueue: Promise<void> = Promise.resolve();

// Until the first graph:connect the connection receives the events of every graph; only the
// open graph's are shown.
const enqueue = (handler: (payload: any) => Promise<void> | void) => (payload: any) => {
  eventQueue = eventQueue.then(() => {
    if (payload.graph !== undefined && payload.graph !== vueModel.currentGraph.id)
      return;
    return handler(payload);
  }).catch((e) => console.log("Graph event failed:", e));
};

// Zooms to fit once the graph:connect snapshot has arrived; the snapshot has no end marker,
// so this waits for a pause in the events that follow a graph:clear. The editor's element
// carries data-settled="true" once the fit is done (06 §4), e.g. for the tests.
let fitTimer: ReturnType<typeof setTimeout> | undefined;
let fitPending = false;
const settled = ref(false);
const scheduleFit = () => {
  if (!fitPending)
    return;
  settled.value = false;
  clearTimeout(fitTimer);
  fitTimer = setTimeout(() => {
    fitPending = false;
    eventQueue = eventQueue.then(async () => {
      await AreaExtensions.zoomAt(editor.area, editor.getNodes());
      settled.value = !fitPending;
    });
  }, 150);
}

// A block by its id in the process (value nodes are not blocks).
const findNodeByName = (name: string): PrimitiveNode | undefined =>
  editor.getNodes().find((n): n is PrimitiveNode => n instanceof PrimitiveNode && n.label === name);

const cloneDescriptors = (ports: Port[]): Port[] => {
  let clone: Port[] = [];
  for (let desc of ports) {
    clone.push({
      id: desc.id,
      type: desc.type,
      description: desc.description,
      addressable: desc.addressable,
      default: desc.default,
      required: desc.required,
      schema: desc.schema,
      values: []
    });
  }
  return clone;
}

const callbacks = {

  onGraphClear: async (payload: ClearEvent): Promise<void> => {
    startTransaction(payload, true);
    vueModel.currentGraph = payload;
    try {
      await editor.clear();
      processOpened(payload.id);
      fitPending = true;
      scheduleFit();
    } finally {
      endTransaction();
    }
  },

  onGraphAddNode: async (payload: AddNodeEvent): Promise<void> => {
    console.log("OnGraphAddNode")
    startTransaction(payload);
    try {
      // A block whose type isn't registered (a placeholder) has no ports.
      let template = componentTemplates[payload.component];
      let inp: Port[] = template ? cloneDescriptors(template.inPorts) : [];
      let outp: Port[] = template ? cloneDescriptors(template.outPorts) : [];
      let node = new PrimitiveNode(payload.id, template ? template.description : "Unknown component", payload.component, inp, outp);
      await editor.addNode(node);
      await editor.area.translate(node.id, payload.metadata);
      scheduleFit();
    } finally {
      endTransaction();
    }
  },

  onGraphRemoveNode: async (payload: RemoveNodeEvent): Promise<void> => {
    startTransaction(payload);
    try {
      let node = findNodeByName(payload.id)?.id;
      if (node !== undefined) {
        await editor.removeNode(node);
        blockRemoved(payload.id);
      }
    } finally {
      endTransaction();
    }
  },

  onGraphRenameNode: async (payload: RenameNodeEvent): Promise<void> => {
    startTransaction(payload);
    try {
      let oldName = payload.from;
      let node = findNodeByName(oldName);
      if (node !== undefined) {
        node.label = payload.to;
        await editor.area.update('node', node.id);
        await renameValues(editor, node);
        blockRenamed(oldName, payload.to);
        editor.blockSelection?.refresh();
      }
    } finally {
      endTransaction();
    }
  },

  onGraphChangeNode: async (payload: ChangeNodeEvent): Promise<void> => {
    startTransaction(payload);
    try {
      let node = findNodeByName(payload.id);
      if (node !== undefined) {
        await editor.setPosition(node.id, payload.metadata as Position);
        await placeValues(editor, node);
      }
    } finally {
      endTransaction();
    }
  },

  onGraphAddEdge: async (p: AddEdgeEvent): Promise<void> => {
    startTransaction(p);
    try {
      let srcNode = findNodeByName(p.src.node); // Ensure these are the Rete Node objects
      let srcPort = p.src.port; // Ensure these are the string IDs of the ports
      let tgtNode = findNodeByName(p.tgt.node);
      let tgtPort = p.tgt.port;

      if (srcNode !== undefined && tgtNode !== undefined) {
        // The Connection class from model.ts extends Classic.Connection
        // Its constructor might be: constructor(source: SourceNode, sourceOutput: SourceOutputKey, target: TargetNode, targetInput: TargetInputKey)
        // Assuming p.src.port and p.tgt.port are the correct key types (string IDs).
        let link = new Connection(srcNode, srcPort as any, tgtNode, tgtPort as any); // Using 'as any' if type checking is problematic with generic keys
        await editor.addConnection(link);
        editor.blockSelection?.refresh();
        connectionsChanged();
        scheduleFit();
      }
    } finally {
      endTransaction();
    }
  },

  onGraphRemoveEdge: async (payload: RemoveEdgeEvent): Promise<void> => {
    startTransaction(payload);
    try {
      let srcNodeId = findNodeByName(payload.src.node)?.id;
      let tgtNodeId = findNodeByName(payload.tgt.node)?.id;

      if (srcNodeId && tgtNodeId) {
        const connections = editor.getConnections();
        const matchingConnections = connections.filter(c =>
            c.source === srcNodeId &&
            c.target === tgtNodeId &&
            c.sourceOutput === payload.src.port &&
            c.targetInput === payload.tgt.port
        );

        if (matchingConnections.length > 0) {
          await editor.removeConnection(matchingConnections[0].id);
          editor.blockSelection?.refresh();
          connectionsChanged();
        }
      }
    } finally {
      endTransaction();
    }
  },

  onGraphChangeEdge: async (payload: ChangeEdgeEvent): Promise<void> => {
    startTransaction(payload);
    try {

    } finally {
      endTransaction();
    }
  },

  onGraphAddInitial: async (payload: AddInitialEvent): Promise<void> => {
    startTransaction(payload);
    try {
      let tgtNode = findNodeByName(payload.tgt.node);
      if (tgtNode !== undefined) {
        await showValue(editor, tgtNode, payload.tgt.port, String(payload.src.data));
        editor.blockSelection?.refresh();
        if (draft.value?.block === tgtNode.id && draft.value.port === payload.tgt.port)
          draft.value = undefined;
      }
    } finally {
      endTransaction();
    }
  },

  onGraphRemoveInitial: async (payload: RemoveInitialEvent): Promise<void> => {
    startTransaction(payload);
    try {
      let tgtNode = findNodeByName(payload.tgt.node);
      if (tgtNode !== undefined) {
        await showValue(editor, tgtNode, payload.tgt.port, undefined);
        editor.blockSelection?.refresh();
      }
    } finally {
      endTransaction();
    }
  },

  onGraphAddInport: async (payload: AddInportEvent): Promise<void> => {
    startTransaction(payload);
    try {

    } finally {
      endTransaction();
    }
  },

  onGraphRemoveInport: async (payload: RemoveInportEvent): Promise<void> => {
    startTransaction(payload);
    try {

    } finally {
      endTransaction();
    }
  },

  onGraphRenameInport: async (payload: RenameInportEvent): Promise<void> => {
    startTransaction(payload);
    try {

    } finally {
      endTransaction();
    }
  },

  onGraphAddOutport: async (payload: AddOutportEvent): Promise<void> => {
    startTransaction(payload);
    try {

    } finally {
      endTransaction();
    }
  },

  onGraphRemoveOutport: async (payload: RemoveOutportEvent): Promise<void> => {
    startTransaction(payload);
    try {

    } finally {
      endTransaction();
    }
  },

  onGraphRenameOutport: async (payload: RenameOutportEvent): Promise<void> => {
    startTransaction(payload);
    try {

    } finally {
      endTransaction();
    }
  },

  onGraphAddGroup: async (payload: AddGroupEvent): Promise<void> => {
    startTransaction(payload);
    try {

    } finally {
      endTransaction();
    }
  },

  onGraphRemoveGroup: async (payload: RemoveGroupEvent): Promise<void> => {
    startTransaction(payload);
    try {

    } finally {
      endTransaction();
    }
  },

  onGraphRenameGroup: async (payload: RenameGroupEvent): Promise<void> => {
    startTransaction(payload);
    try {

    } finally {
      endTransaction();
    }
  },

  onGraphChangeGroup: async (payload: ChangeGroupEvent): Promise<void> => {
    startTransaction(payload);
    try {

    } finally {
      endTransaction();
    }
  }
}

// A value as the event carries it (a string), sent again as its port's type reads it.
const valueData = (value: string, portType: string): string | number | boolean => {
  if (portType === "bool")
    return value === "true";
  if (portType === "string")
    return value;
  const n = Number(value);
  return value.trim() !== "" && Number.isFinite(n) ? n : value;
};

// A line drawn from a free input and dropped on the canvas asks for a fixed value (06 §4.3),
// in a box beside the input until the engine's event shows the value node.
const pane = ref<HTMLElement | null>(null);
const draft = ref<{ block: string, blockLabel: string, port: string, portType: string, x: number, y: number } | undefined>();

// A value node's line picked up from its input, until it is dropped.
let carrying: ValueNode | undefined;

const onDrop = (initial: SocketData, socket: SocketData | null, created: boolean) => {
  const carried = carrying;
  if (carried !== undefined) {
    // Dropped on an input, it makes a connection: connectioncreate moves the value (Rete
    // reports the drop first). Dropped elsewhere, the value is removed.
    if (created)
      return;
    carrying = undefined;
    if (editor.getNode(carried.id) !== undefined)
      vueModel.connection.value?.proto.graph.request_removeinitial(undefined, carried.blockLabel, carried.port, -1);
    return;
  }
  if (!created && socket === null && initial.side === 'input')
    askValue(initial.nodeId, initial.key, initial.element);
};

const askValue = (nodeId: string, port: string, socket: HTMLElement) => {
  const block = editor.getNode(nodeId);
  if (!(block instanceof PrimitiveNode) || pane.value === null)
    return;
  const at = socket.getBoundingClientRect();
  const origin = pane.value.getBoundingClientRect();
  draft.value = {
    block: block.id, blockLabel: block.label, port, portType: block.inputType(port),
    x: at.left - origin.left, y: at.top + at.height / 2 - origin.top,
  };
};

// The context menu (06 §4.4): what it was opened on, where (x, y in the pane; `at` in the
// editor's coordinates, where "Add block…" puts the block).
interface OpenMenu {
  items: MenuItem[];
  target: MenuTarget;
  x: number;
  y: number;
  at: Position;
  node?: string;          // the block or value node, by the editor's id
  port?: string;
  socket?: HTMLElement;   // the input's socket
  connection?: string;
  input?: Point;          // the input whose live value the menu watches (06 §4.7)
}
const menu = ref<OpenMenu | undefined>();
const blockTypes = computed(() => vueModel.components.value.map(c => c.name));


// The most specific of the nested contextmenu events (a block's, then the canvas's) opens it.
const opened = new WeakSet<Event>();

const openMenu = (event: MouseEvent, context: unknown) => {
  event.preventDefault();
  if (opened.has(event) || pane.value === null)
    return;
  opened.add(event);
  draft.value = undefined;
  const origin = pane.value.getBoundingClientRect();
  const where = {x: event.clientX - origin.left, y: event.clientY - origin.top, at: editor.pointAt(event.clientX, event.clientY)};
  let open: Omit<OpenMenu, "items" | "x" | "y" | "at"> | undefined;
  if (context === "root") {
    open = {target: {kind: "canvas", watchingAll: live.all}};
  } else if (context instanceof ValueNode) {
    const value = context as ValueNode;
    const input = {node: value.blockLabel, port: value.port};
    open = {target: {kind: "value", id: value.blockLabel, port: value.port, watched: isWatched(input)}, node: value.id, input};
  } else if (context instanceof PrimitiveNode) {
    const row = event.target instanceof Element ? event.target.closest<HTMLElement>(".input[data-testid^='input-']") : null;
    const port = row?.dataset.testid?.substring("input-".length);
    if (row !== null && port !== undefined && port in context.inputs) {
      const source = inputSources(editor, context)[port] ?? {kind: "default"};
      const input = {node: context.label, port};
      open = {target: {kind: "input", id: context.label, port, source, watched: isWatched(input)}, node: context.id, port,
        socket: row.querySelector<HTMLElement>("[data-testid=input-socket]") ?? undefined, input};
    } else {
      const selection = editor.blockSelection?.selection;
      const selected = selection?.has(context.id) ? selection.ids().length : 0;
      open = {target: {kind: "block", id: context.label, selected, watched: inputsOf(context).some(isWatched)},
        node: context.id};
    }
  } else if (context !== null && typeof context === "object" && "source" in context) {
    const c = context as { id: string, source: string, target: string, targetInput: string };
    const source = editor.getNode(c.source);
    const input = {node: editor.getNode(c.target)?.label ?? "", port: c.targetInput};
    open = isValueNode(source)
      ? {target: {kind: "value", id: source.blockLabel, port: source.port, watched: isWatched(input)}, node: source.id, input}
      : {target: {kind: "connection", watched: isWatched(input)}, connection: c.id, input};
  }
  if (open !== undefined)
    menu.value = {...open, ...where, items: menuFor(open.target)};
};

// The menu key: the menu of the first selected block, else the canvas's, in the middle.
const menuKey = () => {
  const ids = editor.blockSelection?.selection.ids() ?? [];
  const element = ids.length > 0 ? editor.area.nodeViews.get(ids[0])?.element : editor.area.container;
  if (element === undefined)
    return;
  const box = element.getBoundingClientRect();
  const event = new MouseEvent("contextmenu", {clientX: box.left + box.width / 2, clientY: box.top + Math.min(box.height / 2, 20)});
  openMenu(event, ids.length > 0 ? editor.getNode(ids[0]) : "root");
};

const closeMenu = () => {
  menu.value = undefined;
};

const graph = () => vueModel.connection.value?.proto.graph;

const act = async (action: MenuAction) => {
  const m = menu.value;
  closeMenu();
  if (m === undefined)
    return;
  const node = m.node !== undefined ? editor.getNode(m.node) : undefined;
  const block = node instanceof PrimitiveNode ? node : undefined;
  const value = isValueNode(node) ? node : undefined;
  switch (action) {
    case "select-all":
      editor.blockSelection?.selectAll();
      break;
    case "zoom-fit":
      await AreaExtensions.zoomAt(editor.area, editor.getNodes());
      break;
    case "arrange": {
      // Each block moves with its changenode's event.
      const positions = await editor.arrangement?.() ?? new Map<string, Position>();
      positions.forEach((p, id) => {
        const target = editor.getNode(id);
        const now = editor.area.nodeViews.get(id)?.position;
        if (target instanceof PrimitiveNode && (now === undefined || now.x !== p.x || now.y !== p.y))
          graph()?.request_changenode(target.label, {x: p.x, y: p.y});
      });
      break;
    }
    case "rename":
      if (block !== undefined)
        editor.blockSelection?.rename(block.id);
      break;
    case "edit-inputs":
      if (block !== undefined)
        editor.blockSelection?.editInputs(block.id);
      break;
    case "delete-block":
      if (block !== undefined)
        graph()?.request_removenode(block.label);
      break;
    case "delete-selection":
      editor.blockSelection?.removeSelected();
      break;
    case "set-value":
      if (block !== undefined && m.port !== undefined && m.socket !== undefined)
        askValue(block.id, m.port, m.socket);
      break;
    case "change-value": {
      const shown = value ?? (block !== undefined ? editor.getNodes().find((n): n is ValueNode =>
        isValueNode(n) && n.block === block.id && n.port === m.port) : undefined);
      if (shown !== undefined) {
        shown.editRequested = true;
        await editor.area.update("node", shown.id);
      }
      break;
    }
    case "clear-value":
      if (m.target.kind === "value" || m.target.kind === "input")
        graph()?.request_removeinitial(undefined, m.target.id, m.target.port, -1);
      break;
    case "disconnect":
      if (block !== undefined && m.port !== undefined) {
        for (const c of editor.getConnections().filter(c => c.target === block.id && c.targetInput === m.port)) {
          const source = editor.getNode(c.source);
          if (source !== undefined && !isValueNode(source))
            graph()?.request_removeedge(source.label, block.label, c.sourceOutput, c.targetInput, -1, -1);
        }
      }
      break;
    case "watch":
      if (m.input !== undefined)
        watchInputs([m.input]);
      break;
    case "unwatch":
      if (m.input !== undefined)
        unwatch([m.input]);
      break;
    case "watch-inputs":
      if (block !== undefined)
        watchInputs(inputsOf(block));
      break;
    case "unwatch-inputs":
      if (block !== undefined)
        unwatch(inputsOf(block));
      break;
    case "watch-all":
    case "unwatch-all":
      watchAll(action === "watch-all");
      break;
    case "delete-connection": {
      const c = editor.getConnections().find(c => c.id === m.connection);
      const source = c && editor.getNode(c.source);
      const target = c && editor.getNode(c.target);
      if (c !== undefined && source !== undefined && target !== undefined)
        graph()?.request_removeedge(source.label, target.label, c.sourceOutput, c.targetInput, -1, -1);
      break;
    }
  }
};

// A block from the menu's "Add block…", where the menu was opened.
const addBlock = (type: string) => {
  const at = menu.value?.at ?? {x: 0, y: 0};
  closeMenu();
  addBlockAt(type, at);
};

const addBlockAt = (type: string, at: Position) => {
  const name = vueModel.nextName(type, (n) => findNodeByName(n) !== undefined);
  graph()?.request_addnode(name, type, Math.round(at.x), Math.round(at.y));
};

// A block type dragged from the palette (06 §4.5) is added where it is dropped.
const dragOver = (event: DragEvent) => {
  if (event.dataTransfer?.types.includes(BLOCK_TYPE_MIME)) {
    event.preventDefault();
    event.dataTransfer.dropEffect = "copy";
  }
};

const drop = (event: DragEvent) => {
  const type = event.dataTransfer?.getData(BLOCK_TYPE_MIME);
  if (!type)
    return;
  event.preventDefault();
  addBlockAt(type, editor.pointAt(event.clientX, event.clientY));
};

// The socket colors' legend (06 §4.5).
const socketLegend = [
  {kind: "number", label: "number"},
  {kind: "bool", label: "bool"},
  {kind: "string", label: "string"},
];

// The open process was deleted (06 §4.6): the editor shows nothing.
watch(() => vueModel.currentGraphId.value, id => {
  if (id !== "")
    return;
  eventQueue = eventQueue.then(async () => {
    vueModel.pipesDisabled = true;
    try {
      menu.value = undefined;
      draft.value = undefined;
      await editor.clear();
      processOpened("");
    } finally {
      vueModel.pipesDisabled = false;
    }
  });
});

onMounted(() => {
  createEditor(rete.value!).then(() => {
    editor.onDrop = onDrop;
    attachLive(editor);
    editor.onMenuKey = menuKey;
    editor.area.addPipe(context => {
      if (context.type === "contextmenu")
        openMenu(context.data.event, context.data.context);
      return context;
    });
  });
  setTimeout(() => {
    editor.addPipe((ctx) => {
      if (vueModel.pipesDisabled) {
        return ctx;
      }
      let graphProtocol = vueModel.connection.value?.proto.graph;
      switch (ctx.type) {
        case 'nodecreate':
          let componentType = vueModel.selectedComponent;
          let nodeName = vueModel.nextName(componentType, (name) => findNodeByName(name) !== undefined);
          graphProtocol?.request_addnode(nodeName, componentType, 300, 300);
          return undefined;
        case 'noderemove':
          // e.g. the context menu's Delete; the Delete key goes through the selection. A value
          // node's Delete is its connection's removal, which comes first.
          if (!isValueNode(ctx.data))
            graphProtocol?.request_removenode(ctx.data.label);
          return undefined;
        case 'connectioncreate': {
          // A value node's line dropped on an input: the value moves there (picked up from its
          // input), or is copied there (drawn from the value node).
          const value = editor.getNode(ctx.data.source);
          if (isValueNode(value)) {
            const target = editor.getNode(ctx.data.target);
            const carried = carrying === value;
            carrying = undefined;
            if (!(target instanceof PrimitiveNode) || (target.id === value.block && ctx.data.targetInput === value.port))
              return undefined;
            if (carried)
              graphProtocol?.request_removeinitial(undefined, value.blockLabel, value.port, -1);
            graphProtocol?.request_addinitial(valueData(value.value, value.portType), target.label, ctx.data.targetInput, -1, undefined);
            return undefined;
          }
        }
          let fromNode: string | undefined = editor.getNode(ctx.data.source)?.label;
          let fromPort: string = ctx.data.sourceOutput;
          let fromIndex: number = -1;
          let toNode: string | undefined = editor.getNode(ctx.data.target)?.label;
          let toPort: string = ctx.data.targetInput;
          let toIndex: number = -1;
          if (fromNode === undefined || toNode === undefined)
            return undefined;
          graphProtocol?.request_addedge(fromNode, toNode, fromPort, toPort, fromIndex, toIndex, {});
          return undefined;
        case 'connectionremove':
          // A value node's line removed (e.g. the menu's Delete): the input goes back to its
          // default. Picked up to be dragged, nothing is sent until it is dropped (onDrop).
          const fixed = editor.getNode(ctx.data.source);
          const picked = editor.picking;
          editor.picking = false;
          if (isValueNode(fixed)) {
            if (picked)
              carrying = fixed;
            else
              graphProtocol?.request_removeinitial(undefined, fixed.blockLabel, fixed.port, -1);
            return undefined;
          }
          let srcNode = editor.getNode(ctx.data.source)?.label;
          let tgtNode = editor.getNode(ctx.data.target)?.label;
          if (srcNode === undefined || tgtNode === undefined)
            return undefined;
          graphProtocol?.request_removeedge(srcNode, tgtNode, ctx.data.sourceOutput, ctx.data.targetInput, -1, -1);
          return undefined;
      }
      return ctx;
    });
  }, 100);
  let conn = vueModel.connection.value;
  if (conn !== undefined) {
    conn.proto.network.addListener("onNetworkData", dataArrived);
    conn.addClosedListener(connectionLost);
    let graph = conn.proto.graph;
    for (const key in callbacks){
      if (Object.prototype.hasOwnProperty.call(callbacks, key)) {
        const cb = callbacks[key as keyof typeof callbacks];
        graph.addListener(key as keyof GraphProtocol['listeners'], enqueue(cb));
      }
    }
  }
});

</script>

<template>
  <div class="hk-editor-pane" ref="pane">
    <main id="hk-editor" class="rete" ref="rete" data-testid="editor" :data-settled="settled"
          @pointerdown="draft = undefined; closeMenu()" @dragover="dragOver" @drop="drop"></main>
    <ul class="hk-socket-legend" data-testid="socket-legend" aria-label="Port types">
      <li v-for="entry in socketLegend" :key="entry.kind">
        <span class="hk-socket-dot" :class="'hk-socket-' + entry.kind"></span>{{ entry.label }}
      </li>
    </ul>
    <ValueDraft v-if="draft" :key="draft.block + '/' + draft.port" :block="draft.blockLabel" :port="draft.port"
                :port-type="draft.portType" :x="draft.x" :y="draft.y" @close="draft = undefined"/>
    <ContextMenu v-if="menu" :items="menu.items" :x="menu.x" :y="menu.y" :types="blockTypes"
                 @action="act" @add-block="addBlock" @close="closeMenu"/>
  </div>
</template>
