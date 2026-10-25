<script setup lang="ts">

import {computed, nextTick, onMounted, onUnmounted, ref, watch} from "vue";
import {createEditor} from './viewmodel/default';
import {editor} from './viewmodel/default';
import {
  BindingNode, can, componentTemplates, Connection, errorClaims, isPortNode, isValueNode, PortNode, PrimitiveNode, ValueNode, vueModel
} from "../model";
import {SocketData} from "rete-connection-plugin";
import {inputSources, placeValues, renameValues, showValue} from "./values";
import {
  bindingNodes, placePorts, portNodes, processPortNodes, removeBinding, removeBindingMarker, removePort, renamePort, showBinding, showPort
} from "./ports";
import Popup from "@/components/Popup.vue";
import {
  attachLive, blockRemoved, blockRenamed, connectionLost, connectionsChanged, dataArrived, inputsOf, isWatched,
  live, processOpened, unwatch, watch as watchInputs, watchAll
} from "./live";
import ContextMenu from "./ContextMenu.vue";
import {MenuAction, menuFor, MenuItem, MenuTarget} from "./menus";
import {Point} from "@/components/watches";
import {allowed} from "@/components/roles";
import ValueDraft from "./ValueDraft.vue";
import CompositeDialog from "./CompositeDialog.vue";
import BindDialog from "./BindDialog.vue";
import {environment} from "@/components/environment";
import type {Binding} from "@/components/protocols/environment";
import {blockArrived, copySelection, duplicateSelection, hasClip, pasteClipAt, setupCopyPaste} from "./copypaste";
import {eventApplied, setupHistory} from "./history";
import {Link, ProcessPort, SelBlock} from "@/components/composite";
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
  eventQueue = eventQueue.then(async () => {
    if (payload.graph !== undefined && payload.graph !== vueModel.currentGraph.id)
      return;
    await handler(payload);
    eventApplied();   // an undo or redo waiting for its events (06 §4.13)
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
      await placePorts(editor);
      blockArrived(editor, payload.id, node.id);
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
        await placePorts(editor);
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
        for (const marker of portNodes(editor).filter(p => p.block === node!.id))
          await editor.area.update("node", marker.id);
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
        await placePorts(editor);
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
        await placePorts(editor);
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
        await placePorts(editor);
        editor.blockSelection?.refresh();
      }
    } finally {
      endTransaction();
    }
  },

  onGraphAddInport: async (payload: AddInportEvent): Promise<void> => {
    startTransaction(payload);
    try {
      const block = findNodeByName(payload.node);
      if (block !== undefined)
        await showPort(editor, "in", payload.name, block, payload.port);
      portDialogDone("in", payload.name);
    } finally {
      endTransaction();
    }
  },

  onGraphRemoveInport: async (payload: RemoveInportEvent): Promise<void> => {
    startTransaction(payload);
    try {
      await removePort(editor, "in", payload.name);
    } finally {
      endTransaction();
    }
  },

  onGraphRenameInport: async (payload: RenameInportEvent): Promise<void> => {
    startTransaction(payload);
    try {
      await renamePort(editor, "in", payload.from, payload.to);
      portDialogDone("in", payload.to);
    } finally {
      endTransaction();
    }
  },

  onGraphAddOutport: async (payload: AddOutportEvent): Promise<void> => {
    startTransaction(payload);
    try {
      const block = findNodeByName(payload.node);
      if (block !== undefined)
        await showPort(editor, "out", payload.name, block, payload.port);
      portDialogDone("out", payload.name);
    } finally {
      endTransaction();
    }
  },

  onGraphRemoveOutport: async (payload: RemoveOutportEvent): Promise<void> => {
    startTransaction(payload);
    try {
      await removePort(editor, "out", payload.name);
    } finally {
      endTransaction();
    }
  },

  onGraphRenameOutport: async (payload: RenameOutportEvent): Promise<void> => {
    startTransaction(payload);
    try {
      await renamePort(editor, "out", payload.from, payload.to);
      portDialogDone("out", payload.to);
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
  if (!created && socket === null && initial.side === 'input' && can.value.edit)
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
    open = {target: {kind: "canvas", watchingAll: live.all, canPaste: hasClip()}};
  } else if (context instanceof BindingNode) {
    const block = editor.getNode(context.block);
    open = block instanceof PrimitiveNode
      ? {target: {kind: "binding", id: block.label, port: context.port, channel: context.name}, node: block.id, port: context.port}
      : undefined;
  } else if (context instanceof ValueNode) {
    const value = context as ValueNode;
    const input = {node: value.blockLabel, port: value.port};
    open = {target: {kind: "value", id: value.blockLabel, port: value.port, watched: isWatched(input)}, node: value.id, input};
  } else if (context instanceof PortNode) {
    open = {target: {kind: "port", direction: context.direction, name: context.name}, node: context.id};
  } else if (context instanceof PrimitiveNode
      && event.target instanceof Element && event.target.closest(".output[data-testid^='output-']") !== null) {
    const row = event.target.closest<HTMLElement>(".output[data-testid^='output-']")!;
    const port = row.dataset.testid!.substring("output-".length);
    const binding = bindingNodes(editor).find(b => b.block === context.id && b.port === port)?.name;
    open = {target: {kind: "output", id: context.label, port, environment: canBind(), binding}, node: context.id, port};
  } else if (context instanceof PrimitiveNode) {
    const row = event.target instanceof Element ? event.target.closest<HTMLElement>(".input[data-testid^='input-']") : null;
    const port = row?.dataset.testid?.substring("input-".length);
    if (row !== null && port !== undefined && port in context.inputs) {
      const source = inputSources(editor, context)[port] ?? {kind: "default"};
      const input = {node: context.label, port};
      open = {target: {kind: "input", id: context.label, port, source, watched: isWatched(input), environment: canBind()},
        node: context.id, port,
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
    const marker = [source, editor.getNode(c.target)].find(isPortNode);
    const bound = marker instanceof BindingNode ? editor.getNode(marker.block) : undefined;
    open = marker instanceof BindingNode
      ? (bound instanceof PrimitiveNode
        ? {target: {kind: "binding", id: bound.label, port: marker.port, channel: marker.name}, node: bound.id, port: marker.port}
        : undefined)
      : marker !== undefined
      ? {target: {kind: "port", direction: marker.direction, name: marker.name}, node: marker.id}
      : isValueNode(source)
      ? {target: {kind: "value", id: source.blockLabel, port: source.port, watched: isWatched(input)}, node: source.id, input}
      : {target: {kind: "connection", watched: isWatched(input)}, connection: c.id, input};
  }
  // Only what the session may do (06 §4.11); nothing at all: no menu.
  const items = open === undefined ? [] : menuFor(open.target).filter(i => allowed(i.action, can.value));
  if (open !== undefined && items.length > 0)
    menu.value = {...open, ...where, items};
};

// Whether the open process can be bound: an environment, and a process, not a type's graph.
const canBind = () => environment.description !== undefined && vueModel.currentComponent.value === undefined;

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
          if (source instanceof PrimitiveNode)
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
    case "expose-input":
    case "expose-output":
      if (block !== undefined && m.port !== undefined)
        openPortDialog({mode: "add", direction: action === "expose-input" ? "in" : "out", block: block.label, port: m.port});
      break;
    case "bind":
      if (block !== undefined && m.port !== undefined)
        bindDialog.value = {block: block.label, port: m.port, sensor: m.port in block.inputs};
      break;
    case "unbind": {
      const marker = block !== undefined ? bindingNodes(editor).find(b => b.block === block.id && b.port === m.port) : undefined;
      if (block !== undefined && marker !== undefined)
        vueModel.connection.value?.proto.environment.request_unbind(bindingOf(block.label, marker));
      break;
    }
    case "rename-port":
      if (m.target.kind === "port")
        openPortDialog({mode: "rename", direction: m.target.direction, from: m.target.name});
      break;
    case "remove-port":
      if (m.target.kind === "port")
        graph()?.request_removeport(m.target.direction, m.target.name);
      break;
    case "copy":
    case "duplicate":
      // The selection, if the block is in it; else the block alone.
      if (block !== undefined && !editor.blockSelection?.selection.has(block.id))
        editor.blockSelection?.selection.set([block.id]);
      if (action === "copy")
        copySelection(editor);
      else
        duplicateSelection(editor);
      break;
    case "paste":
      pasteClipAt(editor, m.at);
      break;
    case "create-composite": {
      // The selection, if the block is in it; else the block alone.
      const ids = block === undefined ? [] : editor.blockSelection?.selection.has(block.id)
          ? editor.blockSelection.selection.ids() : [block.id];
      openComposite(ids);
      break;
    }
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

// Binding a block port (06 §4.14): the dialog of BindDialog.vue.
const bindDialog = ref<{ block: string, port: string, sensor: boolean } | undefined>();

const bindingOf = (block: string, marker: BindingNode): Binding => marker.index !== undefined && marker.index > 0
    ? {graph: vueModel.currentGraphId.value, block, port: marker.port, env: marker.env, index: marker.index}
    : {graph: vueModel.currentGraphId.value, block, port: marker.port, env: marker.env};

// The environment's events for the open process (02 §3.7): the markers follow them.
const bindingEvents = {
  onBind: async (b: Binding) => {
    const block = findNodeByName(b.block);
    if (block !== undefined)
      await showBinding(editor, block, b.port, b.env, b.index);
  },
  onUnbind: async (b: Binding) => {
    const block = findNodeByName(b.block);
    if (block !== undefined)
      await removeBinding(editor, block, b.port);
  },
  // Every process's bindings (asked for when a process opens): the open one's markers.
  onBindings: async (p: { bindings: Binding[] }) => {
    const graph = vueModel.currentGraph.id;
    const wanted = (p.bindings ?? []).filter(b => b.graph === graph);
    for (const marker of bindingNodes(editor)) {
      const block = editor.getNode(marker.block);
      if (!(block instanceof PrimitiveNode) || !wanted.some(b => b.block === block.label && b.port === marker.port && b.env === marker.env
          && (b.index ?? 0) === (marker.index ?? 0)))
        await removeBindingMarker(editor, marker);
    }
    for (const b of wanted)
      await bindingEvents.onBind(b);
    scheduleFit();   // they come right after a process's snapshot (06 §4)
  },
};

// The process port name dialog (06 §4.8): exposing a block port, or renaming a process port.
// It stays open until the port's event arrives; a refusal is shown in it.
type PortDialog =
  | { mode: "add", direction: "in" | "out", block: string, port: string }
  | { mode: "rename", direction: "in" | "out", from: string };
const portDialog = ref<PortDialog | undefined>();
const portName = ref("");
const portError = ref("");
const portField = ref<HTMLInputElement | null>(null);
let portPending: { direction: "in" | "out", name: string, refusals: string[] } | undefined;

const openPortDialog = async (dialog: PortDialog) => {
  portDialog.value = dialog;
  portError.value = "";
  portPending = undefined;
  if (dialog.mode === "rename") {
    portName.value = dialog.from;
  } else {
    // The port's own name, unless the process already has a port of that direction so named.
    const taken = (n: string) => processPortNodes(editor).some(p => p.direction === dialog.direction && p.name === n);
    portName.value = taken(dialog.port) ? dialog.block + "_" + dialog.port : dialog.port;
  }
  await nextTick();
  portField.value?.focus();
  portField.value?.select();
};

const sendPortDialog = () => {
  const d = portDialog.value;
  if (d === undefined)
    return;
  const name = portName.value;
  const kind = d.direction === "in" ? "inport" : "outport";
  portError.value = "";
  if (d.mode === "rename" && name === d.from) {
    portDialog.value = undefined;
    return;
  }
  portPending = {
    direction: d.direction, name,
    refusals: ["Port exists: " + name, "Invalid id", "Unknown port: " + (d.mode === "add" ? d.block + "." + d.port : d.from),
      "Unknown node: " + (d.mode === "add" ? d.block : ""), "Not permitted: graph:" + (d.mode === "add" ? "add" : "rename") + kind],
  };
  if (d.mode === "add")
    graph()?.request_addport(d.direction, d.block, d.port, name);
  else
    graph()?.request_renameport(d.direction, d.from, name);
};

const portDialogDone = (direction: "in" | "out", name: string) => {
  if (portPending !== undefined && portPending.direction === direction && portPending.name === name) {
    portPending = undefined;
    portDialog.value = undefined;
  }
};

const claimPortRefusal = (message: string): boolean => {
  if (portPending === undefined || !portPending.refusals.includes(message))
    return false;
  portPending = undefined;
  portError.value = message;
  return true;
};
errorClaims.push(claimPortRefusal);
onUnmounted(() => errorClaims.splice(errorClaims.indexOf(claimPortRefusal), 1));

// Creating a composite type from blocks (06 §4.9): the dialog gets the blocks, the connections
// among the process's blocks and its process ports, by the blocks' ids in the process.
const compositeDialog = ref<{ blocks: SelBlock[], links: Link[], processPorts: ProcessPort[] } | undefined>();
const notice = ref("");
let noticeTimer: ReturnType<typeof setTimeout> | undefined;
const showNotice = (text: string) => {
  notice.value = text;
  clearTimeout(noticeTimer);
  noticeTimer = setTimeout(() => notice.value = "", 3000);
};

const openComposite = (ids: string[]) => {
  const blocks: SelBlock[] = ids.flatMap(id => {
    const node = editor.getNode(id);
    const at = editor.area.nodeViews.get(id)?.position;
    if (!(node instanceof PrimitiveNode) || at === undefined)
      return [];
    return [{
      id: node.label, component: node.component, x: Math.round(at.x), y: Math.round(at.y),
      inputs: node.inports.map(p => ({port: p.id, type: p.type ?? ""})),
      outputs: node.outports.map(p => ({port: p.id, type: p.type ?? ""})),
      initials: {...node.initials},
    }];
  });
  if (blocks.length === 0)
    return;
  const links: Link[] = editor.getConnections().flatMap(c => {
    const s = editor.getNode(c.source), t = editor.getNode(c.target);
    return s instanceof PrimitiveNode && t instanceof PrimitiveNode
      ? [{src: {node: s.label, port: c.sourceOutput}, tgt: {node: t.label, port: c.targetInput}}] : [];
  });
  const processPorts: ProcessPort[] = processPortNodes(editor).flatMap(p => {
    const b = editor.getNode(p.block);
    return b instanceof PrimitiveNode ? [{direction: p.direction, name: p.name, node: b.label, port: p.port}] : [];
  });
  compositeDialog.value = {blocks, links, processPorts};
};

const compositeCreated = (type: string) => {
  compositeDialog.value = undefined;
  showNotice(`Created block type “${type}”`);
};

// A block type's component graph, open in the editor (06 §4.9): saving makes it the type again,
// deleting removes the type.
const savingType = ref("");
const typeError = ref("");
const deletingType = ref(false);
const saveType = () => {
  const t = vueModel.currentComponent.value;
  if (t === undefined)
    return;
  typeError.value = "";
  savingType.value = t.id;
  typeBefore = vueModel.components.value.find(c => c.name === t.id);
  vueModel.connection.value?.proto.network.request_persist(t.id);
};
// Saved when the type's component:component arrives: a new entry in the list.
let typeBefore: unknown;
watch(() => vueModel.components.value.find(c => c.name === savingType.value), c => {
  if (c !== undefined && c !== typeBefore && savingType.value !== "") {
    showNotice(`Saved block type “${savingType.value}”`);
    savingType.value = "";
  }
});
const claimTypeError = (message: string, graph?: string): boolean => {
  const t = vueModel.currentComponent.value;
  if (t === undefined || graph !== t.id || (savingType.value === "" && !deletingType.value))
    return false;
  savingType.value = "";
  deletingType.value = false;
  typeError.value = message;
  return true;
};
errorClaims.push(claimTypeError);
onUnmounted(() => errorClaims.splice(errorClaims.indexOf(claimTypeError), 1));
const confirmDeleteType = ref(false);
const deleteType = () => {
  const t = vueModel.currentComponent.value;
  confirmDeleteType.value = false;
  if (t === undefined)
    return;
  typeError.value = "";
  deletingType.value = true;
  vueModel.connection.value?.proto.runtime.request_delete_graph(t.id, t.name);
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
    setupCopyPaste(editor);
    setupHistory(editor, showNotice);
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
          if (ctx.data instanceof PrimitiveNode)
            graphProtocol?.request_removenode(ctx.data.label);
          return undefined;
        case 'connectioncreate': {
          // A process port's line is the port's own (06 §4.8): drawing one changes nothing.
          if (isPortNode(editor.getNode(ctx.data.source)) || isPortNode(editor.getNode(ctx.data.target)))
            return undefined;
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
          if (isPortNode(fixed) || isPortNode(editor.getNode(ctx.data.target)))
            return undefined;
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
    // Applied like the graph's events: the editor's changes are not the user's (no commands).
    for (const [key, handler] of Object.entries(bindingEvents))
      conn.proto.environment.addListener(key as "onBind" | "onUnbind" | "onBindings", enqueue(async (payload: any) => {
        vueModel.pipesDisabled = true;
        try {
          await handler(payload);
        } finally {
          vueModel.pipesDisabled = false;
        }
      }));
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
    <Popup v-if="portDialog" :onOk="sendPortDialog" :onCancel="() => portDialog = undefined"
           :okLabel="portDialog.mode === 'add' ? 'Expose' : 'Rename'">
      <h4>{{ portDialog.mode === 'add'
          ? (portDialog.direction === 'in' ? 'Expose as process input' : 'Expose as process output')
          : 'Rename process port' }}</h4>
      <p v-if="portDialog.mode === 'add'" class="hk-dialog-note">{{ portDialog.block }}.{{ portDialog.port }}</p>
      <div class="hk-field">
        <label class="hk-label" for="hk-port-name">Name</label>
        <input id="hk-port-name" ref="portField" class="hk-input" data-testid="port-name-field" v-model="portName"
               spellcheck="false" autocomplete="off" :aria-invalid="portError !== ''" @keydown.enter.prevent="sendPortDialog"/>
        <p v-if="portError" class="hk-inspector-note" role="alert" data-testid="port-name-error">{{ portError }}</p>
      </div>
    </Popup>
    <BindDialog v-if="bindDialog" :block="bindDialog.block" :port="bindDialog.port" :sensor="bindDialog.sensor"
                @close="bindDialog = undefined"/>
    <CompositeDialog v-if="compositeDialog" :blocks="compositeDialog.blocks" :links="compositeDialog.links"
                     :process-ports="compositeDialog.processPorts" @close="compositeDialog = undefined"
                     @created="compositeCreated"/>
    <div v-if="vueModel.currentComponent.value" class="hk-type-banner" data-testid="type-banner">
      <span>Block type <strong data-testid="type-banner-name">{{ vueModel.currentComponent.value.id }}</strong>
        — changes take effect when saved; a type in use cannot be changed.</span>
      <span class="hk-spacer"></span>
      <span v-if="typeError" class="hk-inspector-note" role="alert" data-testid="type-error">{{ typeError }}</span>
      <button v-if="can.persist" type="button" class="hk-button" data-testid="type-save" @click="saveType">Save type</button>
      <button v-if="can.manage" type="button" class="hk-button danger" data-testid="type-delete" @click="confirmDeleteType = true">Delete type</button>
    </div>
    <Popup v-if="confirmDeleteType" :onOk="deleteType" :onCancel="() => confirmDeleteType = false" okLabel="Delete" :danger="true">
      <h4>Delete block type</h4>
      <p data-testid="delete-type-question">Delete “{{ vueModel.currentComponent.value?.id }}”? Its file is removed too.</p>
    </Popup>
    <div v-if="notice" class="hk-notice" role="status" data-testid="notice">{{ notice }}</div>
    <ContextMenu v-if="menu" :items="menu.items" :x="menu.x" :y="menu.y" :types="blockTypes"
                 @action="act" @add-block="addBlock" @close="closeMenu"/>
  </div>
</template>
