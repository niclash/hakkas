<template>
  <div class="settings" data-testid="settings-panel">
    <p class="intro">
      Changes take effect at once. Those marked <span class="restart-mark">restart</span> power-cycle the emulated
      device: hakkas has to be restarted to see them. <strong>Persist</strong> writes the environment's file.
    </p>
    <section v-for="g in groups" :key="g.title" class="group" data-testid="settings-group" :data-group="g.title">
      <h2 v-if="g.title">{{ g.title }}</h2>
      <div v-for="f in g.fields" :key="f.id" class="field" data-testid="setting" :data-id="f.id">
        <label :for="'f-' + f.id" :title="f.description">{{ f.label }}</label>
        <div class="control">
          <select v-if="f.type === 'enum'" :id="'f-' + f.id" :value="optionKey(values[f.id] ?? null)"
                  :disabled="f.readonly" data-testid="setting-input"
                  @change="set(f, parsed(($event.target as HTMLSelectElement).value))">
            <option v-if="values[f.id] === null || values[f.id] === undefined" :value="optionKey(null)" disabled>— not set —</option>
            <option v-for="o in f.options" :key="optionKey(o.value)" :value="optionKey(o.value)">{{ o.label }}</option>
          </select>
          <NumberField v-else-if="f.type === 'integer' || f.type === 'number'" :id="'f-' + f.id"
                       :value="values[f.id] as number | null" :min="f.min" :max="f.max"
                       :step="f.type === 'integer' ? 1 : undefined" :disabled="f.readonly" data-testid="setting-input"
                       :commit="v => set(f, v)"/>
          <input v-else-if="f.type === 'bool'" :id="'f-' + f.id" type="checkbox" :checked="values[f.id] === true"
                 :disabled="f.readonly" data-testid="setting-input"
                 @change="set(f, ($event.target as HTMLInputElement).checked)">
          <TextField v-else-if="f.type === 'list'" :id="'f-' + f.id" :value="listText(values[f.id])"
                     :disabled="f.readonly" placeholder="comma-separated" data-testid="setting-input"
                     :commit="v => set(f, parseList(v))"/>
          <TextField v-else :id="'f-' + f.id" :value="String(values[f.id] ?? '')" :disabled="f.readonly"
                     :placeholder="f.description" data-testid="setting-input" :commit="v => set(f, v)"/>
          <span v-if="f.unit" class="unit">{{ f.unit }}</span>
          <span v-if="f.restart && !f.readonly" class="restart-mark" data-testid="restart-mark"
                title="Changing this power-cycles the emulated device: hakkas has to be restarted to see it">restart</span>
        </div>
      </div>
    </section>
  </div>
</template>

<script setup lang="ts">
import {computed} from 'vue';
import type {Client} from '../protocol/client';
import type {Field, Json} from '../protocol/types';
import {groupFields, listText, optionKey, parseList} from '../settings';
import NumberField from './NumberField.vue';
import TextField from './TextField.vue';

const props = defineProps<{client: Client}>();
const settings = computed(() => props.client.mirror.settings);
const values = computed(() => settings.value.values);
const groups = computed(() => groupFields(settings.value.fields));

const parsed = (key: string): Json => JSON.parse(key) as Json;

// Sends a `configure` of one field; a refusal is shown as a notice. Settles when answered.
function set(f: Field, value: Json): Promise<unknown> {
  return props.client.request('configure', {values: {[f.id]: value}}).catch((e: Error) =>
    props.client.notify({level: 'error', message: e.message}));
}
</script>

<style scoped>
.settings {
  display: flex;
  flex-direction: column;
  gap: 14px;
  max-width: 760px;
}

.intro {
  margin: 0;
  color: var(--muted);
}

.group {
  background: var(--panel);
  border: 1px solid var(--line);
  border-radius: var(--radius);
  padding: 6px 14px 10px;
}

h2 {
  font-size: 14px;
  margin: 4px 0 8px;
}

.field {
  display: grid;
  grid-template-columns: 220px 1fr;
  align-items: center;
  gap: 12px;
  padding: 4px 0;
}

label {
  color: var(--text);
}

.control {
  display: flex;
  align-items: center;
  gap: 8px;
}

.control select, .control input[type=text] {
  min-width: 240px;
}

.unit {
  color: var(--muted);
}

.restart-mark {
  font-size: 11px;
  color: var(--warn);
  border: 1px solid var(--warn);
  border-radius: 999px;
  padding: 0 7px;
}
</style>
