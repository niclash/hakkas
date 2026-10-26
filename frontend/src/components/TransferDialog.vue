<template>
  <!-- Uploading a process file or a backup (06 §4.16): what it would do, asked first; then its
       progress, the mapping dialog where a process's bindings do not fit, and the report. -->
  <div class="hk-overlay">
    <div class="hk-dialog hk-transfer-dialog" role="dialog" aria-modal="true" aria-labelledby="hk-transfer-title"
         data-testid="transfer-dialog" @keydown.escape="phase !== 'running' && close()">
      <h4 id="hk-transfer-title">Upload {{ fileName }}</h4>
      <p v-if="phase === 'reading'" class="hk-dialog-note">Reading the engine…</p>
      <p v-if="error" class="hk-dialog-error" role="alert" data-testid="transfer-error">{{ error }}</p>

      <template v-if="phase === 'plan' && plan">
        <p class="hk-dialog-note">Uploaded processes stay stopped; each is saved.</p>
        <h5 v-if="plan.processes.length > 0">Processes</h5>
        <ul class="hk-transfer-list">
          <li v-for="p in plan.processes" :key="p.entry.graph.id" data-testid="transfer-process" :data-id="p.entry.graph.id">
            <label>
              <input type="checkbox" v-model="p.include" data-testid="transfer-include"/>
              <span class="hk-transfer-name">{{ p.entry.graph.name }}</span>
              <span :class="{'hk-transfer-warn': p.replaces}" data-testid="transfer-action">{{ p.replaces
                  ? `replaces “${p.replaces.name}”${p.replaces.running ? ", which is running" : ""}` : "new" }}</span>
            </label>
            <span v-if="p.missing.length > 0" class="hk-transfer-warn" data-testid="transfer-missing">
              Not on this engine: {{ p.missing.join(", ") }} (those blocks will be refused)</span>
            <span v-if="p.bindings === 'apply'" data-testid="transfer-bindings">
              Bindings for “{{ p.entry.bindings?.environment }}”</span>
            <span v-else-if="p.bindings === 'no-environment'" class="hk-transfer-warn" data-testid="transfer-bindings">
              Bindings not applied: this engine has no environment</span>
          </li>
        </ul>
        <h5 v-if="plan.types.length > 0">Composite types</h5>
        <ul class="hk-transfer-list">
          <li v-for="t in plan.types" :key="t.type.id" data-testid="transfer-type" :data-id="t.type.id">
            <span class="hk-transfer-name">{{ t.type.id }}</span>
            <span :class="{'hk-transfer-warn': t.action === 'different' || t.action === 'builtin'}" data-testid="transfer-type-action">
              {{ typeText[t.action] }}</span>
          </li>
        </ul>
      </template>

      <p v-if="phase === 'running'" class="hk-dialog-note" role="status" data-testid="transfer-progress">{{ progress }}</p>

      <template v-if="phase === 'done'">
        <ul class="hk-transfer-list">
          <li v-for="(r, i) in report" :key="i" data-testid="transfer-report" :data-id="r.id" :class="{failed: !r.ok}">
            <span class="hk-transfer-name">{{ r.kind === "type" ? r.id : r.name }}</span>
            <span>{{ r.ok ? (r.kind === "process" ? "uploaded, stopped" : "done") : "not done" }}</span>
            <ul v-if="r.notes.length > 0" class="hk-transfer-notes">
              <li v-for="(n, k) in r.notes" :key="k" data-testid="transfer-note">{{ n }}</li>
            </ul>
          </li>
        </ul>
      </template>

      <div class="hk-actions">
        <button v-if="phase !== 'done'" type="button" class="hk-button" data-testid="transfer-cancel"
                :disabled="phase === 'running'" @click="close">Cancel</button>
        <button v-if="phase === 'plan'" type="button" class="hk-button primary" :class="{danger: replacing}"
                data-testid="transfer-upload" :disabled="!plan || !plan.processes.some(p => p.include) && !plan.types.some(t => t.action === 'add')"
                @click="upload">{{ replacing ? "Replace and upload" : "Upload" }}</button>
        <button v-if="phase === 'done'" type="button" class="hk-button primary" data-testid="transfer-close"
                @click="close">Close</button>
      </div>
    </div>
    <MappingDialog v-if="mapping" :file="mapping.file" :rows="mapping.rows" :context="mapping.context"
                   :mismatch="mapping.mismatch" @close="mapped(undefined)" @apply="mapped"/>
  </div>
</template>

<script lang="ts" setup>
import {computed, onMounted, onUnmounted, ref} from "vue";
import type {TransferFile, TypeAction, UploadPlan} from "@/components/transfer";
import {TransferConnection} from "@/components/transfer-connection";
import {planUpload, ReportLine, runUpload} from "@/components/transfer-run";
import type {CheckContext, Entry, MappingRow} from "@/components/bindings";
import MappingDialog from "@/components/floweditor/MappingDialog.vue";

const props = defineProps<{ file: TransferFile, fileName: string }>();
// `open`: the process to open afterwards (the last one uploaded), if any.
const emit = defineEmits<{ close: [open?: string] }>();

const phase = ref<"reading" | "plan" | "running" | "done">("reading");
const error = ref("");
const plan = ref<UploadPlan | undefined>();
const progress = ref("");
const report = ref<ReportLine[]>([]);
const mapping = ref<{ file: string, rows: MappingRow[], context: CheckContext, mismatch?: string,
  done: (e: Entry[] | undefined) => void } | undefined>();
let transfer: TransferConnection | undefined;

const typeText: Record<TypeAction, string> = {
  add: "added",
  same: "already on this engine",
  different: "on this engine and different: this engine's is kept",
  builtin: "a built-in type of this name: kept",
};

const replacing = computed(() => plan.value?.processes.some(p => p.include && p.replaces) ?? false);

onMounted(async () => {
  try {
    transfer = await TransferConnection.open();
    plan.value = await planUpload(transfer, props.file);
    phase.value = "plan";
  } catch (e) {
    error.value = e instanceof Error ? e.message : String(e);
    phase.value = "done";
  }
});

onUnmounted(() => transfer?.close());

const upload = async () => {
  if (transfer === undefined || plan.value === undefined)
    return;
  phase.value = "running";
  report.value = await runUpload(transfer, plan.value, {
    progress: text => progress.value = text,
    mapping: (process, rows, context, mismatch) => new Promise(done =>
        mapping.value = {file: process + ": bindings", rows, context, mismatch, done}),
  });
  transfer.close();
  transfer = undefined;
  phase.value = "done";
};

const mapped = (entries: Entry[] | undefined) => {
  const m = mapping.value;
  mapping.value = undefined;
  m?.done(entries);
};

const close = () => {
  const made = report.value.filter(r => r.kind === "process" && r.ok);
  emit("close", made[made.length - 1]?.id);
};
</script>
