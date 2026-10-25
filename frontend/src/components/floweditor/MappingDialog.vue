<template>
  <!-- The mapping dialog (06 §4.14): a bindings file whose entries do not all apply as they are.
       Each entry is mapped to a block port and a channel, or skipped; Apply replaces the
       process's bindings with the result. -->
  <div class="hk-overlay">
    <form class="hk-dialog hk-mapping-dialog" role="dialog" aria-modal="true" aria-labelledby="hk-mapping-title"
          data-testid="mapping-dialog" @submit.prevent="apply" @keydown.escape="emit('close')">
      <h4 id="hk-mapping-title">Map the bindings file</h4>
      <p class="hk-dialog-note">{{ file }} — {{ rows.length }} entr{{ rows.length === 1 ? "y" : "ies" }}; the process's
        bindings are replaced by what is applied.</p>
      <p v-if="mismatch" class="hk-inspector-note" data-testid="mapping-mismatch">{{ mismatch }}</p>
      <table class="hk-mapping">
        <thead>
        <tr><th scope="col">In the file</th><th scope="col">Block port</th><th scope="col">Channel</th>
          <th scope="col">Skip</th><th scope="col"></th></tr>
        </thead>
        <tbody>
        <tr v-for="(row, i) in rows" :key="i" data-testid="mapping-row" :data-entry="entryText(row)"
            :class="{skipped: row.skip}">
          <td class="hk-composite-ref">{{ entryText(row) }}</td>
          <td>
            <select class="hk-input" data-testid="mapping-port" v-model="row.blockPort" :disabled="row.skip"
                    :aria-label="'Block port for ' + entryText(row)">
              <option value="">— choose —</option>
              <option v-for="p in blockPorts" :key="p.value" :value="p.value">{{ p.label }}</option>
            </select>
          </td>
          <td>
            <select class="hk-input" data-testid="mapping-channel" v-model="row.channel" :disabled="row.skip"
                    :aria-label="'Channel for ' + entryText(row)">
              <option value="">— choose —</option>
              <option v-for="c in channels" :key="c.label" :value="c.label">{{ c.label }} ({{ c.sensor ? "sensor" : "actuator" }})</option>
            </select>
          </td>
          <td><input type="checkbox" v-model="row.skip" data-testid="mapping-skip" :aria-label="'Skip ' + entryText(row)"/></td>
          <td class="hk-mapping-problem" data-testid="mapping-problem">{{ problems[i] ?? "" }}</td>
        </tr>
        </tbody>
      </table>
      <div class="hk-actions">
        <button type="button" class="hk-button" data-testid="mapping-cancel" @click="emit('close')">Cancel</button>
        <button type="submit" class="hk-button primary" data-testid="mapping-apply" :disabled="!ready">Apply</button>
      </div>
    </form>
  </div>
</template>

<script lang="ts" setup>
import {computed, reactive} from "vue";
import {channelLabel, channelsOf, CheckContext, Entry, MappingRow, rowEntry, rowProblems} from "@/components/bindings";

const props = defineProps<{ file: string, rows: MappingRow[], context: CheckContext, mismatch?: string }>();
const emit = defineEmits<{ close: [], apply: [entries: Entry[]] }>();

const rows = reactive(props.rows.map(r => ({...r})));
const problems = computed(() => rowProblems(rows, props.context));
const ready = computed(() => problems.value.every(p => p === undefined));

const entryText = (row: MappingRow) => `${row.entry.block}.${row.entry.port} ⇄ ${channelLabel(row.entry.env, row.entry.index)}`;
const blockPorts = computed(() => Object.entries(props.context.blocks).sort(([a], [b]) => a.localeCompare(b)).flatMap(([block, p]) => [
  ...p.inputs.map(port => ({value: block + "." + port, label: block + "." + port + " (input)"})),
  ...p.outputs.map(port => ({value: block + "." + port, label: block + "." + port + " (output)"})),
]));
const channels = computed(() => channelsOf(props.context.description));

const apply = () => {
  if (!ready.value)
    return;
  emit("apply", rows.filter(r => !r.skip).map(rowEntry).filter((e): e is Entry => e !== undefined));
};
</script>
