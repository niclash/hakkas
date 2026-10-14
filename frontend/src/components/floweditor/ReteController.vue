<script setup lang="ts">

import {onMounted, ref} from "vue";
import {createEditor} from './viewmodel';
import {editor} from './viewmodel/default';
import {componentTemplates, Connection, isValueNode, PrimitiveNode, ValueNode, vueModel} from "../model";
import {SocketData} from "rete-connection-plugin";
import {placeValues, renameValues, showValue} from "./values";
import ValueDraft from "./ValueDraft.vue";
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

onMounted(() => {
  createEditor(rete.value!).then(() => editor.onDrop = onDrop);
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
          @pointerdown="draft = undefined"></main>
    <ValueDraft v-if="draft" :key="draft.block + '/' + draft.port" :block="draft.blockLabel" :port="draft.port"
                :port-type="draft.portType" :x="draft.x" :y="draft.y" @close="draft = undefined"/>
  </div>
</template>
