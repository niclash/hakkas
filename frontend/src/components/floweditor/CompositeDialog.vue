<template>
  <div class="hk-overlay">
    <form class="hk-dialog hk-composite-dialog" role="dialog" aria-modal="true" aria-labelledby="hk-composite-title"
          data-testid="composite-dialog" @submit.prevent="create" @keydown.escape="!busy && emit('close')">
      <h4 id="hk-composite-title">Create composite block type</h4>
      <p class="hk-dialog-note">From {{ blocks.length }} block{{ blocks.length === 1 ? "" : "s" }}:
        {{ blocks.map(b => b.id).join(", ") }}</p>
      <div class="hk-field">
        <label class="hk-label" for="hk-composite-type">Type name</label>
        <input id="hk-composite-type" ref="typeField" class="hk-input" data-testid="composite-type" v-model="type"
               spellcheck="false" autocomplete="off" :aria-invalid="typeProblem !== ''" :disabled="busy"
               @input="touched = true" @blur="touched = true"/>
        <p v-if="typeProblem && touched" class="hk-inspector-note" data-testid="composite-type-problem">{{ typeProblem }}</p>
      </div>
      <div class="hk-field">
        <label class="hk-label" for="hk-composite-description">Description</label>
        <input id="hk-composite-description" class="hk-input" data-testid="composite-description" v-model="description"
               :disabled="busy"/>
      </div>
      <fieldset class="hk-composite-ports" :disabled="busy">
        <legend class="hk-label">Ports of the type — each exposes a port of one of the blocks</legend>
        <table>
          <thead>
          <tr><th scope="col">Expose</th><th scope="col">Block port</th><th scope="col">Name in the type</th></tr>
          </thead>
          <tbody v-for="direction in (['in', 'out'] as const)" :key="direction">
          <tr><th colspan="3" scope="rowgroup" class="hk-composite-group">{{ direction === 'in' ? 'Inputs' : 'Outputs' }}</th></tr>
          <tr v-for="{row, i} in rowsOf(direction)" :key="direction + row.node + '.' + row.port"
              data-testid="composite-port" :data-port="row.node + '.' + row.port" :data-direction="row.direction">
            <td><input type="checkbox" v-model="row.exposed" data-testid="composite-expose"
                       :aria-label="'Expose ' + row.node + '.' + row.port"/></td>
            <td>
              <span class="hk-composite-ref">{{ row.node }}.{{ row.port }}</span>
              <span class="hk-composite-why" v-if="row.why === 'connection'">connected outside</span>
              <span class="hk-composite-why" v-else-if="row.why === 'process-port'">process port</span>
              <span class="hk-composite-why" v-if="row.exposed && row.fixed !== undefined">its fixed value {{ row.fixed }} is left out</span>
            </td>
            <td>
              <input class="hk-input" data-testid="composite-name" v-model="row.name" :disabled="!row.exposed"
                     spellcheck="false" autocomplete="off" :aria-label="'Name of ' + row.node + '.' + row.port"
                     :aria-invalid="problems.has(i)"/>
              <span v-if="problems.has(i)" class="hk-inspector-note" data-testid="composite-name-problem">{{ problems.get(i) }}</span>
            </td>
          </tr>
          </tbody>
        </table>
      </fieldset>
      <p v-if="error" class="hk-dialog-error" role="alert" data-testid="composite-error">{{ error }}</p>
      <div class="hk-actions">
        <button type="button" class="hk-button" data-testid="dialog-cancel" :disabled="busy" @click="emit('close')">Cancel</button>
        <button type="submit" class="hk-button primary" data-testid="dialog-ok"
                :disabled="busy || typeProblem !== ''">
          {{ busy ? "Creating…" : "Create" }}
        </button>
      </div>
    </form>
  </div>
</template>

<script lang="ts" setup>
import {computed, onMounted, onUnmounted, ref, watch} from "vue";
import {errorClaims, vueModel} from "@/components/model";
import {checkPorts, checkTypeName, componentCommands, defaultPorts, Link, ProcessPort, SelBlock} from "@/components/composite";
import {unrecorded} from "./history";

// Creating a composite block type from the selected blocks (06 §4.9): the type's name, its
// description, and which block ports it exposes under which names. The type is built as its
// component graph and persisted (02 §3.4.3); it exists when its component:component arrives.
const props = defineProps<{ blocks: SelBlock[], links: Link[], processPorts: ProcessPort[] }>();
const emit = defineEmits<{ close: [], created: [type: string] }>();

const typeField = ref<HTMLInputElement | null>(null);
const type = ref("user/");
const description = ref("");
const touched = ref(false);
const rows = ref(defaultPorts(props.blocks, props.links, props.processPorts));
const error = ref("");
const busy = ref(false);
let building = "";

const rowsOf = (direction: "in" | "out") =>
    rows.value.map((row, i) => ({row, i})).filter(r => r.row.direction === direction);
const typeProblem = computed(() => checkTypeName(type.value, vueModel.components.value.map(c => c.name)));
const problems = computed(() => checkPorts(rows.value));

const create = () => {
  touched.value = true;
  error.value = "";
  if (typeProblem.value !== "" || problems.value.size > 0)
    return;
  const t = type.value;
  const label = t.substring(t.lastIndexOf("/") + 1);
  const conn = vueModel.connection.value;
  if (conn === undefined)
    return;
  busy.value = true;
  building = t;
  // The type's graph is built, not edited: nothing of it is undone (06 §4.13).
  unrecorded(() => {
    for (const c of componentCommands(t, label, description.value, props.blocks, props.links, rows.value)) {
      if (c.protocol === "graph")
        conn.proto.graph.request_raw(c.command, {...c.payload});
      else
        conn.proto.network.request_persist(t);
    }
  });
};

// Done when the type is announced.
watch(() => vueModel.components.value.some(c => c.name === building), made => {
  if (made && busy.value) {
    busy.value = false;
    emit("created", building);
  }
});

// A refusal about the type being built: shown here, and its half-built graph dropped.
const claim = (message: string, graph?: string): boolean => {
  if (!busy.value || graph !== building)
    return false;
  busy.value = false;
  error.value = message;
  const label = building.substring(building.lastIndexOf("/") + 1);
  vueModel.connection.value?.proto.runtime.request_delete_graph(building, label);
  return true;
};

// The deletion of a graph that was never made is refused too: not shown.
const quiet = (message: string, graph?: string): boolean =>
    !busy.value && graph === building && building !== "" && message.startsWith("Unknown graph");

onMounted(() => {
  errorClaims.push(claim, quiet);
  typeField.value?.focus();
});
onUnmounted(() => {
  errorClaims.splice(errorClaims.indexOf(claim), 1);
  errorClaims.splice(errorClaims.indexOf(quiet), 1);
});
</script>
