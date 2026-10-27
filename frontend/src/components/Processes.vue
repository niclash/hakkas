<template>
  <section class="hk-panel">
    <div class="hk-panel-header">
      <h2 class="hk-panel-title">Processes</h2>
      <span class="hk-panel-buttons">
        <button v-if="can.manage" class="hk-button small" type="button" data-testid="process-new" @click="openNewProcessPopup()">New…</button>
        <button v-if="mayUpload" class="hk-button small" type="button" data-testid="process-upload"
                title="Upload process files, a backup or type files" @click="uploadInput?.click()">Upload…</button>
        <button class="hk-button small" type="button" data-testid="process-map" :aria-pressed="mapOpen"
                title="The processes and the links between them" @click="openMap()">Map</button>
        <button class="hk-button small" type="button" data-testid="process-backup" :disabled="downloading"
                title="Download every process, its bindings and the composite types" @click="download('all')">Back up</button>
      </span>
      <input ref="uploadInput" type="file" accept=".json,application/json" multiple class="hk-hidden" data-testid="process-upload-file"
             @change="pickUpload"/>
    </div>
    <p v-if="transferMessage" class="hk-inspector-note" role="status" data-testid="transfer-message">{{ transferMessage }}</p>
    <ul class="hk-list">
      <ProcessItem v-for="[id, graph] in vueModel.graphs.value" :key="id" :id="id" :graph="graph"
                   :active="id === current" :now="now"
                   @open="$emit('onSelection', id)" @start="network()?.request_start(id); refresh(id)"
                   @stop="network()?.request_stop(id); refresh(id)" @save="save(id)" @delete="deleting = id"
                   @download="download([id])"
                   @rename="to => rename(id, graph.name, to)"/>
      <li v-if="vueModel.graphs.value.size === 0" class="hk-empty">No processes</li>
    </ul>
  </section>

  <Popup v-if="showNewProcessPopup" :onOk="handleCreateProcess" :onCancel="cancelNewProcessPopup" okLabel="Create">
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

  <Popup v-if="deleting !== undefined" :onOk="confirmDelete" :onCancel="() => deleting = undefined"
         okLabel="Delete" :danger="true">
    <h4>Delete process</h4>
    <p data-testid="delete-question">
      Delete “{{ vueModel.graphs.value.get(deleting)?.name }}”? It is stopped, and its saved file is removed too.
    </p>
  </Popup>

  <Popup v-if="saveError !== ''" :onOk="() => saveError = ''" :onCancel="() => saveError = ''" :cancelLabel="null">
    <h4>The process was not saved</h4>
    <p class="hk-dialog-error" role="alert" data-testid="save-error">{{ saveError }}</p>
  </Popup>

  <TransferDialog v-if="uploading" :file="uploading.set.file" :sources="uploading.set.sources" :fileName="uploading.name"
                  @close="uploaded"/>

  <div v-if="notice !== ''" class="hk-notice" role="status" data-testid="notice">{{ notice }}</div>
</template>

<script lang="ts" setup>
import {computed, onMounted, onUnmounted, ref, watch} from "vue";
import {can, errorClaims, vueModel} from './model';
import {mapOpen, openMap} from "@/components/map/map";
import TransferDialog from "@/components/TransferDialog.vue";
import {fileName, parseUpload, transferText, UploadSet} from "@/components/transfer";
import {readFile} from "@/components/transfer-run";
import Popup from "@/components/Popup.vue";
import ProcessItem from "@/components/ProcessItem.vue";
import {isSaveFailure, newProcessId} from "@/components/processes";

// The processes come from the session's network:status, runtime:new_graph and
// runtime:delete_graph events (model.ts attachModel); every action here is a command, and the
// list changes with the events (06 §4.6).
const props = defineProps<{
  current: string
}>();

const emit = defineEmits<{
  (e: 'onSelection', value: string): void
}>();

const network = () => vueModel.connection.value?.proto.network;

// network:started/stopped and graph:rename reach only the process's subscribers (02 §4.1), and
// a process in the list need not be open: its status, which every client receives, follows.
const refresh = (id: string) => network()?.request_getstatus(id);

const rename = (id: string, from: string, to: string) => {
  vueModel.connection.value?.proto.graph.request_rename(id, from, to);
  refresh(id);
};

// Saving (D1): the engine answers a successful save with the process's status, saved; a
// failure with a network:error, shown in a dialog that stays until it is dismissed.
const saving = new Set<string>();
const notice = ref("");
const saveError = ref("");
let noticeTimer: ReturnType<typeof setTimeout> | undefined;

const save = (id: string) => {
  saving.add(id);
  network()?.request_persist(id);
};

watch(() => [...vueModel.graphs.value.entries()].map(([id, g]) => [id, g.unsaved, g.uptimeAt] as const), entries => {
  for (const [id, unsaved] of entries) {
    if (saving.has(id) && !unsaved) {
      saving.delete(id);
      notice.value = `Saved “${vueModel.graphs.value.get(id)?.name}”`;
      clearTimeout(noticeTimer);
      noticeTimer = setTimeout(() => notice.value = "", 3000);
    }
  }
});

const claimSaveFailure = (message: string): boolean => {
  for (const id of saving) {
    if (isSaveFailure(message, id)) {
      saving.delete(id);
      saveError.value = message;
      return true;
    }
  }
  return false;
};

// Ctrl+S saves the open process.
const keydown = (event: KeyboardEvent) => {
  if (event.key === "s" && (event.ctrlKey || event.metaKey) && !event.altKey && !event.shiftKey) {
    event.preventDefault();
    if (props.current !== "" && can.value.persist && vueModel.graphs.value.has(props.current))
      save(props.current);
  }
};

// ---- Download, back up, upload (06 §4.16) --------------------------------------------------

const transferMessage = ref("");
const downloading = ref(false);
const uploadInput = ref<HTMLInputElement | null>(null);
const uploading = ref<{ set: UploadSet, name: string } | undefined>();
const mayUpload = computed(() => can.value.manage && can.value.edit && can.value.persist);

const saveFile = (text: string, name: string) => {
  const url = URL.createObjectURL(new Blob([text], {type: "application/json"}));
  const a = document.createElement("a");
  a.href = url;
  a.download = name;
  a.click();
  URL.revokeObjectURL(url);
};

const download = async (ids: string[] | "all") => {
  transferMessage.value = "";
  downloading.value = true;
  try {
    const file = await readFile(ids);
    const name = ids === "all"
        ? fileName(`${String(vueModel.runtime.value?.label ?? "hakkas")} ${file.saved.substring(0, 10)}`, ".backup.json")
        : fileName(vueModel.graphs.value.get(ids[0])?.name ?? ids[0], ".process.json");
    saveFile(transferText(file), name);
  } catch (e) {
    transferMessage.value = (ids === "all" ? "Back up: " : "Download: ") + (e instanceof Error ? e.message : String(e));
  } finally {
    downloading.value = false;
  }
};

const pickUpload = async (event: Event) => {
  const input = event.target as HTMLInputElement;
  const files = [...(input.files ?? [])];
  input.value = "";
  if (files.length === 0)
    return;
  transferMessage.value = "";
  const parsed = parseUpload(await Promise.all(files.map(async f => ({name: f.name, text: await f.text()}))));
  if (typeof parsed === "string") {
    transferMessage.value = parsed;
    return;
  }
  uploading.value = {set: parsed, name: files.length === 1 ? files[0].name : `${files.length} files`};
};

const uploaded = (open?: string) => {
  uploading.value = undefined;
  if (open !== undefined && vueModel.graphs.value.has(open))
    emit("onSelection", open);
};

// Deleting asks first, naming the process.
const deleting = ref<string | undefined>();
const confirmDelete = () => {
  const id = deleting.value;
  deleting.value = undefined;
  const graph = id !== undefined ? vueModel.graphs.value.get(id) : undefined;
  if (id !== undefined && graph !== undefined)
    vueModel.connection.value?.proto.runtime.request_delete_graph(id, graph.name);
};

// A process created here is opened when its runtime:new_graph arrives.
const showNewProcessPopup = ref(false);
const newProcessName = ref("");
const newProcessDescription = ref("");
const created = ref<string | undefined>();

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
  created.value = newProcessId();
  vueModel.connection.value?.proto.runtime.request_new_graph(newProcessName.value, newProcessDescription.value, "", created.value);
  showNewProcessPopup.value = false;
};

watch(() => created.value !== undefined && vueModel.graphs.value.has(created.value), exists => {
  if (exists && created.value !== undefined) {
    emit("onSelection", created.value);
    created.value = undefined;
  }
});

const cancelNewProcessPopup = () => {
  showNewProcessPopup.value = false;
};

// The running processes' uptime counts on.
const now = ref(Date.now());
let clock: ReturnType<typeof setInterval> | undefined;

onMounted(() => {
  errorClaims.push(claimSaveFailure);
  document.addEventListener("keydown", keydown);
  clock = setInterval(() => now.value = Date.now(), 15000);
});

onUnmounted(() => {
  errorClaims.splice(errorClaims.indexOf(claimSaveFailure), 1);
  document.removeEventListener("keydown", keydown);
  clearInterval(clock);
  clearTimeout(noticeTimer);
});
</script>
