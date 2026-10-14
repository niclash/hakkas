<template>
  <section class="hk-panel">
    <div class="hk-panel-header">
      <h2 class="hk-panel-title">Processes</h2>
      <button class="hk-button small" type="button" @click="openNewProcessPopup()">New...</button>
    </div>
    <ul class="hk-list">
      <li v-for="[id, graph] in vueModel.graphs.value" :key="id" @click="$emit('onSelection', id)"
          class="hk-list-item" data-testid="process" :class="{ active: id === current }" :title="graph.description">
        <span class="hk-state" :class="{ running: graph.running }" :title="graph.running ? 'Running' : 'Stopped'"></span>{{ graph.name }}
      </li>
      <li v-if="vueModel.graphs.value.size === 0" class="hk-empty">No processes</li>
    </ul>
  </section>

  <Popup v-if="showNewProcessPopup" :onOk="handleCreateProcess" :onCancel="cancelNewProcessPopup">
    <h4>Create new process</h4>
    <div class="hk-field">
      <label class="hk-label" for="newProcessName">Name</label>
      <input type="text" class="hk-input" id="newProcessName" v-model="newProcessName"/>
    </div>
    <div class="hk-field">
      <label class="hk-label" for="newProcessDescription">Description</label>
      <input type="text" class="hk-input" id="newProcessDescription" v-model="newProcessDescription"/>
    </div>
  </Popup>
</template>

<script lang="ts" setup>
import {ref} from "vue";
import {vueModel} from './model';
import Popup from "@/components/Popup.vue";

// The processes come from the session's network:status, runtime:new_graph and
// runtime:delete_graph events (model.ts attachModel).
defineProps<{
  current: string
}>();

const showNewProcessPopup = ref(false);
const newProcessName = ref("");
const newProcessDescription = ref("");

const openNewProcessPopup = () => {
  newProcessName.value = "";
  newProcessDescription.value = "";
  showNewProcessPopup.value = true;
};

const handleCreateProcess = () => {
  if (!newProcessName.value || newProcessName.value.trim() === "") {
    alert("Process name cannot be empty.");
    return;
  }
  vueModel.connection.value?.proto.runtime.request_new_graph(newProcessName.value, newProcessDescription.value, "");
  showNewProcessPopup.value = false;
};

const cancelNewProcessPopup = () => {
  showNewProcessPopup.value = false;
};

defineEmits<{
  (e: 'onSelection', value: string): void
}>();

</script>
