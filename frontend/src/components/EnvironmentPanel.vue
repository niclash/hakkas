<template>
  <!-- The engine's environment (02 §3.7, 06 §4.14): its carrier, modules and channels, what each
       channel is bound to, and the open process's bindings file. -->
  <section v-if="d" class="hk-panel hk-env" data-testid="env-panel">
    <div class="hk-panel-header">
      <h2 class="hk-panel-title">Environment</h2>
    </div>
    <p class="hk-env-name" data-testid="env-name" :title="d.description">{{ d.name }}</p>
    <p v-if="d.carrier" class="hk-env-carrier" data-testid="env-carrier" :title="d.carrier.name && d.carrier.type">{{ carrierText }}</p>
    <ul v-if="d.carrier && d.carrier.comm.length > 0" class="hk-env-comm">
      <li v-for="c in d.carrier.comm" :key="c.slot" data-testid="env-comm" :title="c.name && c.type">{{ commText(c) }}</li>
    </ul>
    <details v-for="group in groups" :key="group.key" class="hk-group" data-testid="env-module" :data-slot="group.slot">
      <summary :title="group.title">{{ group.label }}</summary>
      <ul class="hk-env-channels">
        <li v-for="c in group.channels" :key="c.label" data-testid="env-channel" :data-channel="c.label"
            :data-bound="c.bound" :class="{bound: c.bound !== ''}">
          <span class="hk-env-channel">{{ c.label }}</span>
          <span class="hk-env-kind">{{ c.sensor ? "in" : "out" }}{{ c.unit ? ", " + c.unit : "" }}</span>
          <span class="hk-env-bound">{{ c.bound || "free" }}</span>
        </li>
      </ul>
    </details>
    <div v-if="process !== ''" class="hk-env-actions">
      <button v-if="can.edit" type="button" class="hk-button" data-testid="env-load" @click="fileInput?.click()">
        Load bindings file…</button>
      <button type="button" class="hk-button" data-testid="env-save" @click="save">Save bindings file…</button>
      <input ref="fileInput" type="file" accept=".json,application/json" class="hk-hidden" data-testid="env-file"
             @change="load"/>
    </div>
    <p v-if="message" class="hk-inspector-note" role="status" data-testid="env-message">{{ message }}</p>
    <MappingDialog v-if="mapping" :file="mapping.file" :rows="mapping.rows" :context="mapping.context"
                   :mismatch="mapping.mismatch" @close="mapping = undefined" @apply="applyMapped"/>
  </section>
</template>

<script lang="ts" setup>
import {computed, ref} from "vue";
import {can, vueModel} from "@/components/model";
import {bindingsOf, boundTo, environment, refreshBindings} from "@/components/environment";
import type {EnvComm} from "@/components/protocols/environment";
import {
  bindingsFileText, channelsOfPort, checkEntries, CheckContext, Entry, initialRows, MappingRow, parseBindingsFile
} from "@/components/bindings";
import {checkContext} from "@/components/floweditor/bind-context";
import MappingDialog from "@/components/floweditor/MappingDialog.vue";

const d = computed(() => environment.description);
// The open process (a component graph has no bindings).
const process = computed(() =>
    vueModel.currentComponent.value?.id === vueModel.currentGraphId.value ? "" : vueModel.currentGraphId.value);
const processName = (id: string) => vueModel.graphs.value.get(id)?.name ?? id;

const carrierText = computed(() => {
  const c = d.value?.carrier;
  if (c === undefined)
    return "";
  const slots = c.slots + c.builtin.length;
  return [c.name ?? c.type, c.vendor, c.revision ? "rev. " + c.revision : "", c.serial ? "serial " + c.serial : "",
    `${slots} I/O slot${slots === 1 ? "" : "s"}`].filter(s => s).join(" · ");
});

const commText = (c: EnvComm) => {
  const ip = c.ip ? [c.ip.mode, c.ip.address && c.ip.address + (c.ip.prefix !== undefined ? "/" + c.ip.prefix : "")]
      .filter(s => s).join(" ") : "";
  const rs = c.rs485 ? [c.rs485.bitrate, c.rs485.parity].filter(s => s !== undefined).join(" ") : "";
  const lora = c.lorawan ? "Dev EUI " + (c.lorawan.devEui ?? "?") + (c.lorawan.appKeySet ? ", App Key set" : ", no App Key") : "";
  return [`Comm ${c.slot}: ${c.name ?? c.type ?? "?"}`, ip, c.wifi?.ssid ? "SSID " + c.wifi.ssid : "", rs, lora].filter(s => s).join(" · ");
};

// The channels by module (in slot order), then the ports no module lists.
const groups = computed(() => {
  const desc = d.value;
  if (desc === undefined)
    return [];
  const channel = (port: string) => channelsOfPort(desc, port).map(c => ({
    ...c,
    bound: boundTo(c.env, c.index).map(b => (b.graph === vueModel.currentGraphId.value ? "" : processName(b.graph) + ": ")
        + b.port).join(", "),
  }));
  const listed = new Set<string>();
  const out = desc.modules.map((m, i) => {
    m.ports.forEach(p => listed.add(p));
    return {
      key: "m" + i, slot: m.slot ?? "",
      label: (m.slot !== undefined ? `Slot ${m.slot}: ` : "") + (m.name ?? m.type ?? "module") + (m.builtin ? ", built in" : ""),
      title: [m.name !== undefined && m.type, m.vendor, m.revision && "rev. " + m.revision, m.serial && "serial " + m.serial, m.documentation].filter(s => s).join(" · "),
      channels: m.ports.flatMap(channel),
    };
  });
  const rest = [...desc.outPorts, ...desc.inPorts].map(p => p.id).filter(p => !listed.has(p));
  if (rest.length > 0)
    out.push({key: "rest", slot: "", label: desc.modules.length > 0 ? "Other ports" : "Ports", title: "", channels: rest.flatMap(channel)});
  return out;
});

// ---- The bindings file ------------------------------------------------------------------

const message = ref("");
const fileInput = ref<HTMLInputElement | null>(null);
const mapping = ref<{ file: string, rows: MappingRow[], context: CheckContext, mismatch?: string } | undefined>();

const save = () => {
  const desc = d.value;
  if (desc === undefined || process.value === "")
    return;
  const text = bindingsFileText(desc.name, bindingsOf(process.value));
  const url = URL.createObjectURL(new Blob([text], {type: "application/json"}));
  const a = document.createElement("a");
  a.href = url;
  a.download = processName(process.value).replace(/[^A-Za-z0-9_.-]+/g, "_") + ".bindings.json";
  a.click();
  URL.revokeObjectURL(url);
};

// The process's bindings replaced by entries: one step (06 §4.13), the engine's events show it.
const replaceWith = (entries: Entry[]) => {
  const env = vueModel.connection.value?.proto.environment;
  const graph = process.value;
  for (const b of bindingsOf(graph))
    env?.request_unbind(b);
  for (const e of entries)
    env?.request_bind({graph, ...e});
};

const load = async (event: Event) => {
  const input = event.target as HTMLInputElement;
  const file = input.files?.[0];
  input.value = "";
  const desc = d.value, conn = vueModel.connection.value, graph = process.value;
  if (file === undefined || desc === undefined || conn === undefined || graph === "")
    return;
  message.value = "";
  const parsed = parseBindingsFile(await file.text());
  if (typeof parsed === "string") {
    message.value = file.name + ": " + parsed;
    return;
  }
  // Against what is bound now (other processes' bindings come with no event, 02 §4.1).
  await refreshBindings(conn);
  const context = checkContext(desc, graph, environment.bindings.filter(b => b.graph !== graph));
  const warnings = checkEntries(parsed.bindings, context);
  const mismatch = parsed.environment !== desc.name
      ? `Written for the environment “${parsed.environment}”; this one is “${desc.name}”.` : undefined;
  // Any warning brings up the mapping dialog (04 §4); else it applies as it is.
  if (mismatch === undefined && warnings.every(w => w === undefined)) {
    replaceWith(parsed.bindings);
    message.value = `${file.name}: ${parsed.bindings.length} binding${parsed.bindings.length === 1 ? "" : "s"} applied`;
    return;
  }
  mapping.value = {file: file.name, rows: initialRows(parsed.bindings, context), context, mismatch};
};

const applyMapped = (entries: Entry[]) => {
  const name = mapping.value?.file ?? "";
  mapping.value = undefined;
  replaceWith(entries);
  message.value = `${name}: ${entries.length} binding${entries.length === 1 ? "" : "s"} applied`;
};
</script>
