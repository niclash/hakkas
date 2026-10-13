<template>
  <div>
    <!-- Button to toggle the main process list (existing) -->
    <button
        class="btn btn-primary"
        type="button"
        :aria-expanded="!view.isCollapsed.value"
        aria-controls="processesCollapse"
        @click="toggleCollapse"
    >
      ☰
    </button>
    <!-- "New..." button to trigger the popup -->
    <button class="btn-sm" type="button" @click="openNewProcessPopup()">New...</button>

    <div class="collapse" :class="{ 'show': !view.isCollapsed.value }" id="processesCollapse">
      <h3>Processes</h3>
      <ul class="list-group">
        <li v-for="[id, graph] in vueModel.graphs.value" :key="id" @click="$emit('onSelection', id)"
            class="list-group-item" data-testid="process" :class="{ active: id === current }" :title="graph.description">
          <span class="state" :class="{ running: graph.running }"></span>{{ graph.name }}
        </li>
      </ul>
    </div>
  </div>

  <!-- New Process Popup -->
  <Popup v-if="showNewProcessPopup" :onOk="handleCreateProcess" :onCancel="cancelNewProcessPopup">
    <h4>Create New Process</h4>
    <div class="form-group">
      <label for="newProcessName">Name:</label>
      <input type="text" class="form-control" id="newProcessName" v-model="newProcessName"/>
    </div>
    <div class="form-group">
      <label for="newProcessDescription">Description:</label>
      <input type="text" class="form-control" id="newProcessDescription" v-model="newProcessDescription"/>
    </div>
    <!-- Add some spacing for the buttons -->
    <div style="margin-top: 15px;"></div>
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

const view = {
  isCollapsed: ref(false),
}

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

const toggleCollapse = () => {
  view.isCollapsed.value = !view.isCollapsed.value;
};

defineEmits<{
  (e: 'onSelection', value: string): void
}>();

</script>

<style scoped>
.list-group-item {
  cursor: pointer;
}

.state {
  display: inline-block;
  width: 8px;
  height: 8px;
  border-radius: 50%;
  margin-right: 8px;
  background-color: #aaa;
}

.state.running {
  background-color: #2e9d5b;
}

h3 {
  padding-top: 15px;
  font-size: large;
}

/* Basic styling for form elements within the popup */
.form-group {
  margin-bottom: 10px;
}

.form-group label {
  display: block;
  margin-bottom: 5px;
}

.form-group input.form-control { /* Basic input styling */
  width: 100%;
  padding: 8px;
  border: 1px solid #ccc;
  border-radius: 4px;
  box-sizing: border-box;
}
</style>
