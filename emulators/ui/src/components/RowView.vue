<template>
  <tr class="row" :class="row.kind" data-testid="row" :data-key="row.key" :data-kind="row.kind">
    <td :title="row.description">
      <div class="label">
        <span class="mono">{{ row.label }}</span>
        <span v-if="row.description" class="desc">{{ row.description }}</span>
      </div>
    </td>

    <td class="value" data-testid="value">
      <template v-if="row.kind === 'output'">
        <span v-if="indicator === 'lamp'" class="lamp" :class="{on: value === true}" data-testid="lamp"
              :aria-label="value === true ? 'on' : 'off'"></span>
        <template v-else-if="indicator === 'bar' || indicator === 'meter'">
          <meter :min="row.min" :max="row.max" :value="typeof value === 'number' ? value : row.min"></meter>
          <span class="num">{{ shown }}</span>
        </template>
        <span v-else class="num">{{ shown }}</span>
      </template>
      <template v-else-if="row.kind !== 'signal'">
        <span class="num">{{ shown }}</span><span v-if="row.unit && typeof value === 'number'" class="unit">{{ row.unit }}</span>
      </template>
    </td>

    <td data-testid="control">
      <div class="control">
      <template v-if="row.kind === 'input' && isConstant">
        <template v-if="row.type === 'bool'">
          <button v-if="row.control === 'button'" class="momentary" :class="{on: source === true}"
                  data-testid="button" @pointerdown="setSource(true)" @pointerup="setSource(false)"
                  @pointerleave="pressed && setSource(false)" @keydown.space.prevent="setSource(true)"
                  @keyup.space.prevent="setSource(false)">press</button>
          <label v-else class="switch">
            <input type="checkbox" :checked="source === true" data-testid="toggle"
                   @change="setSource(($event.target as HTMLInputElement).checked)">
            <span>{{ source === true ? 'on' : 'off' }}</span>
          </label>
        </template>
        <template v-else>
          <input v-if="hasSlider" type="range" :min="row.min" :max="row.max" :step="step"
                 :value="dragging ?? source" data-testid="slider"
                 @input="slide(Number(($event.target as HTMLInputElement).value))"
                 @change="release(Number(($event.target as HTMLInputElement).value))">
          <NumberField :value="source as number" :min="row.min" :max="row.max" :step="step" data-testid="number"
                       :commit="v => setSource(v)"/>
        </template>
      </template>
      <span v-else-if="row.kind === 'input' || row.kind === 'signal'" class="summary" data-testid="summary">
        {{ summary }}
      </span>
      <button v-if="row.kind === 'input' || row.kind === 'signal'" class="edit" data-testid="edit-source"
              :class="{active: editing}" :title="editing ? 'Close' : 'Set a function of time'"
              @click="editing = !editing">ƒ</button>
      </div>
    </td>

    <td class="noise">
      <label v-if="hasNoise" class="noise-field" title="Noise: a random value within ± this, anew at each scan turn">
        ±<NumberField :value="noise as number" :min="0" :step="step" data-testid="noise"
                      :commit="v => set(`${row.key}.noise`, v)"/>
      </label>
    </td>
  </tr>
  <tr v-if="editing" class="editor-row">
    <td colspan="4">
      <SourceEditor :row="row" :source="source" @apply="apply" @cancel="editing = false"/>
    </td>
  </tr>
</template>

<script setup lang="ts">
import {computed, ref} from 'vue';
import type {Client} from '../protocol/client';
import type {Json, Row} from '../protocol/types';
import {describeSource, formatValue, kindOf, stepOf} from '../sources';
import NumberField from './NumberField.vue';
import SourceEditor from './SourceEditor.vue';

const props = defineProps<{row: Row, client: Client}>();
const values = computed(() => props.client.mirror.values);

const value = computed(() => values.value[props.row.key]);
const source = computed(() => values.value[`${props.row.key}.source`]);
const noise = computed(() => values.value[`${props.row.key}.noise`]);
const hasNoise = computed(() => `${props.row.key}.noise` in values.value);
const isConstant = computed(() => kindOf(source.value) === 'constant');
const shown = computed(() => formatValue(value.value, props.row));
const summary = computed(() => describeSource(source.value, props.row.unit));
const indicator = computed(() => props.row.indicator ?? (props.row.type === 'bool' ? 'lamp' : 'number'));
const step = computed(() => stepOf(props.row));
const hasSlider = computed(() => props.row.control !== 'number' && props.row.min !== undefined
  && props.row.max !== undefined);

const editing = ref(false);
const dragging = ref<number | undefined>(undefined);
const pressed = ref(false);

// Sends a `set`; a refusal is shown as a notice. Settles when it is answered.
function set(key: string, value: Json): Promise<unknown> {
  return props.client.request('set', {key, value}).catch((e: Error) =>
    props.client.notify({level: 'error', message: e.message}));
}

function setSource(v: Json): Promise<unknown> {
  if (typeof v === 'boolean' && props.row.control === 'button') {
    if (pressed.value === v)
      return Promise.resolve();
    pressed.value = v;
  }
  return set(`${props.row.key}.source`, v);
}

// A slider sends while it is dragged, at most every 50 ms, and its last position when let go.
let lastSent = 0;
let trailing: ReturnType<typeof setTimeout> | undefined;
function slide(v: number): void {
  dragging.value = v;
  clearTimeout(trailing);
  const wait = lastSent + 50 - Date.now();
  if (wait <= 0) {
    lastSent = Date.now();
    setSource(v);
  } else {
    trailing = setTimeout(() => slide(v), wait);
  }
}

function release(v: number): void {
  clearTimeout(trailing);
  dragging.value = undefined;
  setSource(v);
}

function apply(s: Json): void {
  editing.value = false;
  void setSource(s);
}
</script>

<style scoped>
td {
  padding: 5px 14px;
  border-top: 1px solid var(--line);
  vertical-align: middle;
}

.label {
  display: flex;
  flex-direction: column;
}

.mono, .num {
  font-family: var(--mono);
}

.desc {
  font-size: 12px;
  color: var(--muted);
}

.value {
  white-space: nowrap;
}

.unit {
  margin-left: 4px;
  color: var(--muted);
}

.control {
  display: flex;
  align-items: center;
  gap: 10px;
}

input[type=range] {
  flex: 1;
  max-width: 360px;
  accent-color: var(--accent);
}

.summary {
  flex: 1;
  color: var(--accent);
  font-family: var(--mono);
  font-size: 13px;
}

.edit {
  margin-left: auto;      /* the same place in every row */
  font-style: italic;
  font-family: Georgia, serif;
  padding: 0 9px;
}

.edit.active {
  background: var(--accent-soft);
  border-color: var(--accent);
}

.switch {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  cursor: pointer;
}

.switch input {
  accent-color: var(--accent);
  width: 16px;
  height: 16px;
}

.momentary.on {
  background: var(--accent);
  color: #fff;
}

.lamp {
  display: inline-block;
  width: 14px;
  height: 14px;
  border-radius: 50%;
  background: var(--lamp-off);
  vertical-align: middle;
}

.lamp.on {
  background: var(--lamp-on);
  box-shadow: 0 0 6px var(--lamp-on);
}

meter {
  width: 90px;
  margin-right: 8px;
  vertical-align: middle;
}

.noise-field {
  color: var(--muted);
  white-space: nowrap;
}

.noise-field input {
  width: 5.5em;
  margin-left: 2px;
}

.editor-row td {
  border-top: none;
  background: var(--accent-soft);
}
</style>
