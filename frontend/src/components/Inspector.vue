<template>
  <aside ref="panel" class="hk-panel hk-inspector" data-testid="inspector" aria-label="Inspector">
    <div class="hk-panel-header">
      <h2 class="hk-panel-title">Inspector</h2>
    </div>

    <p v-if="view.kind === 'none'" class="hk-empty" data-testid="inspector-empty">
      Select a block to see its type and ports.
    </p>

    <template v-else-if="view.kind === 'one'">
      <form class="hk-inspector-name" @submit.prevent="commitName">
        <label class="hk-inspector-heading" for="hk-inspector-name">Name</label>
        <input id="hk-inspector-name" ref="nameField" class="hk-input" data-testid="inspector-name"
               v-model="draft" spellcheck="false" autocomplete="off" :aria-invalid="nameError !== ''"
               aria-describedby="hk-inspector-name-error"
               @focus="editing = true" @blur="blurName" @keydown.escape.stop.prevent="revertName"/>
        <p v-if="nameError" id="hk-inspector-name-error" class="hk-inspector-note" role="alert"
           data-testid="inspector-name-error">{{ nameError }}</p>
      </form>
      <div class="hk-inspector-type" data-testid="inspector-type">{{ view.component }}</div>
      <p v-if="!view.known" class="hk-inspector-note" data-testid="inspector-unknown">
        This block type is not registered: the block keeps its connections but does nothing.
      </p>
      <p v-else-if="view.description" class="hk-inspector-description" data-testid="inspector-description">
        {{ view.description }}
      </p>
      <section v-for="group in portGroups" :key="group.title" class="hk-inspector-ports"
               :data-testid="'inspector-' + group.key">
        <h4 class="hk-inspector-heading">{{ group.title }}</h4>
        <dl>
          <div v-for="port in group.ports" :key="shownNode + '/' + port.id" class="hk-inspector-port"
               data-testid="inspector-port" :data-port="port.id">
            <dt>
              <span class="hk-inspector-port-id" data-testid="port-id">{{ port.id }}</span>
              <span class="hk-inspector-port-type" data-testid="port-type">{{ port.type }}</span>
              <button v-if="group.key === 'inputs'" type="button" class="hk-icon-button hk-watch" data-testid="port-watch"
                      :aria-pressed="watched(port.id)" :aria-label="'Watch the live value of ' + port.id"
                      :title="watched(port.id) ? 'Stop watching' : 'Watch the live value'"
                      @click="toggleWatch(port.id)">◉</button>
            </dt>
            <dd v-if="port.description" data-testid="port-description">{{ port.description }}</dd>
            <dd v-if="group.key === 'inputs' && watched(port.id)" class="hk-inspector-live" data-testid="port-live"
                :class="{stale: liveOf(port.id)?.stale}">
              Live: {{ liveText(port) }}
            </dd>
            <dd v-if="group.key === 'inputs'">
              <InputValue :block="view.id" :port="port"/>
            </dd>
          </div>
        </dl>
      </section>
      <div class="hk-actions">
        <button type="button" class="hk-button" data-testid="inspector-delete" @click="removeSelected">Delete</button>
      </div>
    </template>

    <template v-else>
      <h3 class="hk-inspector-id" data-testid="inspector-count">{{ view.ids.length }} blocks selected</h3>
      <p class="hk-inspector-list" data-testid="inspector-ids">{{ view.ids.join(", ") }}</p>
      <div class="hk-actions">
        <button type="button" class="hk-button" data-testid="inspector-delete" @click="removeSelected">
          Delete {{ view.ids.length }} blocks
        </button>
      </div>
    </template>
  </aside>
</template>

<script lang="ts" setup>
import {computed, nextTick, onMounted, onUnmounted, ref, watch} from "vue";
import {errorClaims, vueModel} from "@/components/model";
import {PendingRename} from "@/components/renaming";
import {inspect, PortView} from "@/components/inspector";
import {ComponentEvent} from "@/components/protocols/component";
import {editor} from "@/components/floweditor/viewmodel/default";
import InputValue from "@/components/InputValue.vue";
import {isWatched, liveValue, unwatch, watch as watchInputs} from "@/components/floweditor/live";
import {formatLive} from "@/components/watches";

// The selected blocks (06 §4.2): the view's own state, so nothing here changes the process
// except through the engine's commands.
const view = computed(() => {
  const types: Record<string, ComponentEvent> = {};
  vueModel.components.value.forEach(c => types[c.name] = c as ComponentEvent);
  return inspect(vueModel.selectedBlocks.value, types);
});

const portGroups = computed(() => {
  const v = view.value;
  if (v.kind !== "one")
    return [];
  const groups: { key: string, title: string, ports: PortView[] }[] = [];
  if (v.inputs.length > 0)
    groups.push({key: "inputs", title: "Inputs", ports: v.inputs});
  if (v.outputs.length > 0)
    groups.push({key: "outputs", title: "Outputs", ports: v.outputs});
  return groups;
});

const removeSelected = () => editor.blockSelection?.removeSelected();

// Live values of the block's inputs (06 §4.7).
const point = (port: string) => ({node: shownId() ?? "", port});
const watched = (port: string) => isWatched(point(port));
const liveOf = (port: string) => liveValue(point(port));
const liveText = (port: PortView) => {
  const v = liveOf(port.id);
  return v === undefined ? "waiting…" : formatLive(port.type, v.data);
};
const toggleWatch = (port: string) => watched(port) ? unwatch([point(port)]) : watchInputs([point(port)]);

// Renaming the block (06 §4.2): the field holds what is typed; the block's id changes only
// with the engine's graph:renamenode event, and a refusal is shown under the field.
const nameField = ref<HTMLInputElement | null>(null);
const draft = ref("");
const editing = ref(false);
const nameError = ref("");
const pending = new PendingRename();

const shownId = () => view.value.kind === "one" ? view.value.id : undefined;

// The block shown, by the editor's id (its renames keep it), and its id when last shown.
const shownNode = ref<string | undefined>();
let shownLabel: string | undefined;

watch(view, () => {
  const blocks = vueModel.selectedBlocks.value;
  const node = blocks.length === 1 ? blocks[0].node : undefined;
  const id = shownId();
  const before = shownLabel;
  shownLabel = id;
  if (node !== shownNode.value) {
    // Another block, or none. Clicking a block keeps the focus in the field (the editor
    // takes the pointer), so a name being typed for the previous block is sent here.
    if (editing.value && before !== undefined && draft.value !== before && nameError.value === "" && !pending.active)
      vueModel.connection.value?.proto.graph.request_renamenode(before, draft.value);
    shownNode.value = node;
    pending.cancel();
    nameError.value = "";
    draft.value = id ?? "";
    nameField.value?.blur();
    return;
  }
  // The same block, renamed: by our rename (then it is done), or by someone else.
  pending.shown(id);
  if (!editing.value || draft.value === id) {
    draft.value = id ?? "";
    nameError.value = "";
  }
}, {immediate: true});

const commitName = () => {
  const id = shownId();
  if (id === undefined || pending.active)
    return;
  nameError.value = "";
  if (draft.value === id)
    return;
  pending.sent(id, draft.value);
  vueModel.connection.value?.proto.graph.request_renamenode(id, draft.value);
};

// Leaving the field sends a changed name; after a refusal, it puts the block's id back.
const blurName = () => {
  editing.value = false;
  if (nameError.value !== "")
    revertName();
  else
    commitName();
};

const revertName = () => {
  pending.cancel();
  draft.value = shownId() ?? "";
  nameError.value = "";
  nameField.value?.blur();
};

const claim = (message: string): boolean => {
  if (!pending.refusal(message))
    return false;
  nameError.value = message;
  return true;
};

// F2 and the context menu's Rename: the field, its text selected, once it is shown.
let focusWanted = false;
const focusName = () => {
  if (!focusWanted || nameField.value === null)
    return;
  focusWanted = false;
  nameField.value.focus();
  nameField.value.select();
};
watch(() => vueModel.renameRequest.value, () => {
  focusWanted = true;
  focusName();
}, {flush: "post"});
watch(view, focusName, {flush: "post"});

// The block menu's "Edit inputs": the first input that takes a value.
const panel = ref<HTMLElement | null>(null);
let inputsWanted = false;
const focusInputs = () => {
  const first = panel.value?.querySelector<HTMLElement>("[data-testid=value-field], [data-testid=value-switch]");
  if (!inputsWanted || first == null)
    return;
  inputsWanted = false;
  first.focus();
};
watch(() => vueModel.inputsRequest.value, () => {
  inputsWanted = true;
  focusInputs();
}, {flush: "post"});
watch(view, focusInputs, {flush: "post"});

onMounted(() => errorClaims.push(claim));
onUnmounted(() => errorClaims.splice(errorClaims.indexOf(claim), 1));
</script>
