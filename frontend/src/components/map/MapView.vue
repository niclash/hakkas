<template>
  <!-- The process map (06 §4.17): the processes and the links between them. -->
  <div class="hk-editor-pane hk-map-pane">
    <main id="hk-map" class="rete" ref="container" data-testid="map" :data-settled="settled"
          @pointerdown="menu = undefined"></main>
    <p v-if="processMap.loaded && processMap.state.processes.size === 0" class="hk-map-empty" data-testid="map-empty">
      No processes
    </p>
    <div v-if="notice" class="hk-notice" role="status" data-testid="map-notice">{{ notice }}</div>
    <ContextMenu v-if="menu" :items="menu.items" :x="menu.x" :y="menu.y" :types="[]"
                 @action="act" @close="menu = undefined"/>
  </div>
</template>

<script lang="ts" setup>
import {nextTick, onMounted, onUnmounted, ref, watch} from "vue";
import {NodeEditor} from "rete";
import {Area2D, AreaExtensions, AreaPlugin} from "rete-area-plugin";
import {ConnectionPlugin, Presets as ConnectionPresets} from "rete-connection-plugin";
import {Presets as VuePresets, VueArea2D, VuePlugin} from "rete-vue-plugin";
import {can} from "@/components/model";
import {environment} from "@/components/environment";
import ContextMenu from "@/components/floweditor/ContextMenu.vue";
import {mapMenuFor, MenuAction, MenuItem} from "@/components/floweditor/menus";
import type {ProcessLink} from "@/components/protocols/link";
import {changeLink, openProcess, processMap, redoLink, setMapNotice, undoLink} from "./map";
import {LinkConnection, MapSchemes, ProcessNode} from "./nodes";
import {layoutMap} from "./state";
import MapNodeView from "./MapNodeView.vue";
import MapSocketView from "./MapSocketView.vue";
import MapLinkView from "./MapLinkView.vue";

type AreaExtra = Area2D<MapSchemes> | VueArea2D<MapSchemes>;

const container = ref<HTMLElement | null>(null);
// The layout is done and the view fitted (as the editor's data-settled, for the tests).
const settled = ref(false);
const notice = ref("");
const menu = ref<{ items: MenuItem[], x: number, y: number, link?: ProcessLink, graph?: string } | undefined>();

let editor: NodeEditor<MapSchemes> | undefined;
let area: AreaPlugin<MapSchemes, AreaExtra> | undefined;
// The map is being drawn from the state: its own changes, not the user's.
let drawing = false;

let noticeTimer: ReturnType<typeof setTimeout> | undefined;
const showNotice = (text: string) => {
  notice.value = text;
  clearTimeout(noticeTimer);
  noticeTimer = setTimeout(() => notice.value = "", 3000);
};

const graphOf = (node: ProcessNode | undefined) => node?.graph;
const linkOf = (c: { source: string, sourceOutput: string | number | symbol, target: string, targetInput: string | number | symbol }): ProcessLink | undefined => {
  const src = graphOf(editor?.getNode(c.source)), tgt = graphOf(editor?.getNode(c.target));
  return src === undefined || tgt === undefined ? undefined
      : {src: {graph: src, port: String(c.sourceOutput)}, tgt: {graph: tgt, port: String(c.targetInput)}};
};

// ---- Drawing: the whole map again from the state, laid out and fitted ----------------------

// Columns along the links (state.ts layoutMap), each process measured as drawn.
async function layout() {
  const e = editor, a = area;
  if (e === undefined || a === undefined)
    return;
  await nextTick();
  const size = (graph: string) => {
    const node = e.getNode("process:" + graph);
    const el = node && a.nodeViews.get(node.id)?.element.firstElementChild as HTMLElement | null | undefined;
    return {width: el?.offsetWidth || node?.width || 340, height: el?.offsetHeight || node?.height || 100};
  };
  for (const [graph, at] of layoutMap(processMap.state, size))
    await a.translate("process:" + graph, at);
}

let drawQueue: Promise<void> = Promise.resolve();
let drawTimer: ReturnType<typeof setTimeout> | undefined;
// Drawn after a pause in the messages (a deletion comes as several).
const scheduleDraw = () => {
  settled.value = false;
  clearTimeout(drawTimer);
  drawTimer = setTimeout(() => drawQueue = drawQueue.then(draw).catch(e => console.log("Map drawing failed:", e)), 50);
};

async function draw() {
  const e = editor, a = area;
  if (e === undefined || a === undefined)
    return;
  drawing = true;
  try {
    await e.clear();
    const nodes = new Map<string, ProcessNode>();
    for (const [graph, ports] of processMap.state.processes) {
      const node = new ProcessNode(graph, ports.inports, ports.outports);
      nodes.set(graph, node);
      await e.addNode(node);
    }
    for (const l of processMap.state.links) {
      const s = nodes.get(l.src.graph), t = nodes.get(l.tgt.graph);
      if (s !== undefined && t !== undefined && s.hasOutput(l.src.port) && t.hasInput(l.tgt.port))
        await e.addConnection(new LinkConnection(s, l.src.port as never, t, l.tgt.port as never));
    }
    await layout();
    await AreaExtensions.zoomAt(a, e.getNodes());
  } finally {
    drawing = false;
  }
  settled.value = true;
}

watch(() => processMap.version, scheduleDraw);
// The ports' channels.
watch(() => environment.bindings, () => {
  if (area !== undefined)
    for (const n of editor?.getNodes() ?? [])
      void area.update("node", n.id);
}, {deep: true});

// ---- Menus and keys ---------------------------------------------------------------------

function openMenu(event: MouseEvent, context: unknown) {
  event.preventDefault();
  const box = container.value!.getBoundingClientRect();
  const at = {x: event.clientX - box.left, y: event.clientY - box.top};
  if (context instanceof ProcessNode) {
    menu.value = {items: mapMenuFor({kind: "process"}), ...at, graph: context.graph};
  } else if (context !== null && typeof context === "object" && "source" in context) {
    // A connection's menu: the area hands over its data.
    const link = linkOf(context as LinkConnection);
    const items = mapMenuFor({kind: "link", edit: can.value.edit});
    menu.value = link !== undefined && items.length > 0 ? {items, ...at, link} : undefined;
  }
  // The canvas ("root") has no menu; its event also follows a process's or a link's.
}

function act(action: MenuAction) {
  const m = menu.value;
  menu.value = undefined;
  if (action === "open-process" && m?.graph !== undefined)
    openProcess(m.graph);
  else if (action === "delete-link" && m?.link !== undefined)
    changeLink(false, m.link);
}

const typing = (event: Event) => {
  const target = event.target as HTMLElement | null;
  return target !== null && (target.isContentEditable || ["INPUT", "TEXTAREA", "SELECT"].includes(target.tagName));
};
const onKey = (event: KeyboardEvent) => {
  if (typing(event) || !(event.ctrlKey || event.metaKey) || event.altKey)
    return;
  const key = event.key.toLowerCase();
  if (key === "z" && !event.shiftKey) {
    event.preventDefault();
    undoLink();
  } else if ((key === "z" && event.shiftKey) || key === "y") {
    event.preventDefault();
    redoLink();
  }
};

// ---- Setup ------------------------------------------------------------------------------

onMounted(async () => {
  const e = new NodeEditor<MapSchemes>();
  const a = new AreaPlugin<MapSchemes, AreaExtra>(container.value!);
  const connection = new ConnectionPlugin<MapSchemes, AreaExtra>();
  const render = new VuePlugin<MapSchemes, AreaExtra>();
  editor = e;
  area = a;
  e.use(a);
  a.use(render);
  a.use(connection);
  connection.addPreset(ConnectionPresets.classic.setup());
  // Links are drawn and pulled only by who may edit (06 §4.11).
  connection.addPipe(context => context.type === "connectionpick" && !can.value.edit ? undefined : context);
  render.addPreset(VuePresets.classic.setup({
    customize: {
      node: () => MapNodeView,
      socket: () => MapSocketView,
      connection: () => MapLinkView,
    },
  }));
  // A line drawn or pulled sends the command; the map changes with its event (02 §1.1).
  e.addPipe(ctx => {
    if (drawing)
      return ctx;
    if (ctx.type === "connectioncreate" || ctx.type === "connectionremove") {
      const link = linkOf(ctx.data);
      if (link !== undefined)
        changeLink(ctx.type === "connectioncreate", link);
      return undefined;
    }
    return ctx;
  });
  a.addPipe(context => {
    if (context.type === "contextmenu")
      openMenu(context.data.event, context.data.context);
    return context;
  });
  setMapNotice(showNotice);
  document.addEventListener("keydown", onKey);
  scheduleDraw();
});

onUnmounted(() => {
  document.removeEventListener("keydown", onKey);
  clearTimeout(drawTimer);
  area?.destroy();
  editor = undefined;
  area = undefined;
});
</script>
