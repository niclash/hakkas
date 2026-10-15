<template>
  <li class="hk-list-item hk-process" data-testid="process" :data-id="id" :class="{active, unsaved: graph.unsaved, stopped: !graph.running}"
      :title="graph.description" @click="!renaming && emit('open')">
    <span class="hk-state" :class="{running: graph.running}" data-testid="process-state"
          :title="graph.running ? 'Running' : 'Stopped'"></span>
    <input v-if="renaming" ref="field" class="hk-input hk-process-name-field" data-testid="process-name-field"
           v-model="draft" :aria-label="'New name of ' + graph.name" spellcheck="false" autocomplete="off"
           @click.stop @keydown.enter.prevent="commit" @keydown.escape.stop.prevent="cancel" @blur="commit"/>
    <span v-else class="hk-process-name" data-testid="process-name" :title="graph.name" @dblclick.stop="can.edit && rename()">{{ graph.name }}</span>
    <span v-if="graph.unsaved" class="hk-unsaved" data-testid="process-unsaved" title="Unsaved changes"
          aria-label="Unsaved changes">●</span>
    <span class="hk-process-actions">
      <button v-if="can.control" type="button" class="hk-icon-button" data-testid="process-run"
              :aria-label="(graph.running ? 'Stop ' : 'Start ') + graph.name" :title="graph.running ? 'Stop' : 'Start'"
              @click.stop="graph.running ? emit('stop') : emit('start')">{{ graph.running ? "■" : "▶" }}</button>
      <button v-if="can.persist" type="button" class="hk-icon-button" data-testid="process-save" :disabled="!graph.unsaved"
              :aria-label="'Save ' + graph.name" title="Save (Ctrl+S)" @click.stop="emit('save')">
        <svg viewBox="0 0 16 16" width="14" height="14" aria-hidden="true"><path fill="currentColor"
            d="M2 1h10l3 3v10a1 1 0 0 1-1 1H2a1 1 0 0 1-1-1V2a1 1 0 0 1 1-1zm1 1v4h8V2H3zm5 7a2 2 0 1 0 0 4 2 2 0 0 0 0-4z"/></svg>
      </button>
      <button v-if="can.edit" type="button" class="hk-icon-button" data-testid="process-rename" :aria-label="'Rename ' + graph.name"
              title="Rename" @click.stop="rename">✎</button>
      <button v-if="can.manage" type="button" class="hk-icon-button" data-testid="process-delete" :aria-label="'Delete ' + graph.name"
              title="Delete" @click.stop="emit('delete')">&times;</button>
    </span>
    <div v-if="active" class="hk-process-details" data-testid="process-details">
      <span data-testid="process-uptime">{{ graph.running ? 'Running for ' + uptime : 'Stopped' }}</span>
      <span v-if="graph.description" class="hk-process-description" data-testid="process-description">{{ graph.description }}</span>
    </div>
  </li>
</template>

<script lang="ts" setup>
import {computed, nextTick, ref} from "vue";
import {can, Graph} from "@/components/model";
import {formatUptime, uptimeNow} from "@/components/processes";

// One process in the list (06 §4.6): its running mark, name, unsaved mark and actions. The name
// is renamed in place; what is shown changes only with the engine's events.
const props = defineProps<{ id: string, graph: Graph, active: boolean, now: number }>();
const emit = defineEmits<{
  open: [], start: [], stop: [], save: [], delete: [], rename: [to: string]
}>();

const uptime = computed(() => formatUptime(uptimeNow(props.graph.uptime, props.graph.uptimeAt, props.graph.running, props.now)));

const renaming = ref(false);
const draft = ref("");
const field = ref<HTMLInputElement | null>(null);

const rename = async () => {
  draft.value = props.graph.name;
  renaming.value = true;
  await nextTick();
  field.value?.focus();
  field.value?.select();
};

// Enter or leaving the field sends a changed name.
const commit = () => {
  if (!renaming.value)
    return;
  renaming.value = false;
  if (draft.value !== props.graph.name)
    emit("rename", draft.value);
};

const cancel = () => {
  renaming.value = false;
};

defineExpose({rename});
</script>
