<template>
  <div class="fields" data-testid="source-fields">
    <label class="kind">
      <span v-if="caption" class="caption">{{ caption }}</span>
      <select :value="kind" data-testid="kind" @change="choose(($event.target as HTMLSelectElement).value as SourceKind)">
        <option v-for="k in kinds" :key="k" :value="k">{{ KIND_LABELS[k] }}</option>
      </select>
    </label>

    <template v-if="kind === 'constant'">
      <label v-if="range.type === 'bool'">
        <input type="checkbox" :checked="modelValue === true" data-testid="p-value"
               @change="emit('update:modelValue', ($event.target as HTMLInputElement).checked)"> on
      </label>
      <label v-else>value <input type="number" :value="modelValue" :step="step" data-testid="p-value"
                                 @change="num($event, v => emit('update:modelValue', v))">{{ unitText }}</label>
    </template>

    <template v-else-if="kind === 'pulse'">
      <SourceFields caption="frequency" :model-value="p.frequency!" :kinds="NUMBER_KINDS"
                    :range="{type: 'number', min: 0, max: 100}" unit="Hz" data-testid="p-frequency"
                    @update:model-value="v => param('frequency', v)"/>
      <SourceFields caption="duty" :model-value="p.duty!" :kinds="NUMBER_KINDS"
                    :range="{type: 'number', min: 0, max: 100}" unit="%" data-testid="p-duty"
                    @update:model-value="v => param('duty', v)"/>
    </template>

    <template v-else-if="kind === 'sequence'">
      <ol class="steps">
        <li v-for="(s, i) in steps" :key="i" data-testid="step">
          <input type="number" min="0" step="0.1" :value="s[0]" title="seconds" data-testid="step-seconds"
                 @change="num($event, v => setStep(i, 0, v))"> s at
          <input v-if="range.type === 'bool'" type="checkbox" :checked="s[1] === true" data-testid="step-value"
                 @change="setStep(i, 1, ($event.target as HTMLInputElement).checked)">
          <input v-else type="number" :value="s[1]" :step="step" data-testid="step-value"
                 @change="num($event, v => setStep(i, 1, v))">{{ unitText }}
          <button type="button" class="small" title="Remove" :disabled="steps.length === 1" @click="removeStep(i)">−</button>
        </li>
      </ol>
      <button type="button" class="small" data-testid="add-step" @click="addStep">+ step</button>
      <label><input type="checkbox" :checked="p.repeat === true" @change="flag($event, 'repeat')"> repeat</label>
    </template>

    <template v-else>
      <template v-for="f in numberFields" :key="f.key">
        <label v-if="f.bool">{{ f.label }}
          <input type="checkbox" :checked="p[f.key] === true" :data-testid="`p-${f.key}`" @change="flag($event, f.key)">
        </label>
        <label v-else>{{ f.label }}
          <input type="number" :value="p[f.key]" :min="f.min" :max="f.max" :step="f.step ?? step"
                 :data-testid="`p-${f.key}`" @change="num($event, v => param(f.key, v))">{{ f.unit ?? unitText }}
        </label>
      </template>
      <label v-if="kind === 'ramp'">
        <input type="checkbox" :checked="p.repeat === true" data-testid="p-repeat" @change="flag($event, 'repeat')"> repeat
      </label>
    </template>
  </div>
</template>

<script setup lang="ts">
import {computed} from 'vue';
import type {Json, JsonObject, SourceKind} from '../protocol/types';
import {KIND_LABELS, kindOf, newSource, NUMBER_KINDS, paramsOf, stepOf} from '../sources';

interface Range {
  type: 'number' | 'bool' | 'pulse';
  min?: number;
  max?: number;
  resolution?: number;
}

const props = defineProps<{modelValue: Json, kinds: SourceKind[], range: Range, unit?: string, caption?: string}>();
const emit = defineEmits<{'update:modelValue': [Json]}>();

const kind = computed(() => kindOf(props.modelValue) ?? 'constant');
const p = computed(() => paramsOf(props.modelValue));
const step = computed(() => stepOf(props.range));
const unitText = computed(() => props.unit ? ` ${props.unit}` : '');
const steps = computed(() => (p.value.steps ?? []) as [number, Json][]);

interface NumberField {
  key: string;
  label: string;
  unit?: string;
  min?: number;
  max?: number;
  step?: number;
  bool?: boolean;
}

const numberFields = computed((): NumberField[] => {
  const bool = props.range.type === 'bool';
  switch (kind.value) {
  case 'sine':
    return [{key: 'offset', label: 'offset'}, {key: 'amplitude', label: 'amplitude'},
      {key: 'period', label: 'period', unit: ' s', min: 0, step: 0.1}];
  case 'square':
    return [{key: 'low', label: 'low', bool}, {key: 'high', label: 'high', bool},
      {key: 'period', label: 'period', unit: ' s', min: 0, step: 0.1},
      {key: 'duty', label: 'duty', unit: ' %', min: 0, max: 100, step: 1}];
  case 'ramp':
    return [{key: 'from', label: 'from'}, {key: 'to', label: 'to'},
      {key: 'period', label: 'period', unit: ' s', min: 0, step: 0.1}];
  case 'counter':
    return [{key: 'rate', label: 'rate', unit: ' /s', min: 0, step: 0.1}];
  default:
    return [];
  }
});

function choose(k: SourceKind): void {
  emit('update:modelValue', newSource(k, props.range,
    kind.value === 'constant' ? props.modelValue : undefined));
}

function replace(params: JsonObject): void {
  emit('update:modelValue', {[kind.value]: params});
}

function param(key: string, v: Json): void {
  replace({...p.value, [key]: v});
}

function flag(e: Event, key: string): void {
  param(key, (e.target as HTMLInputElement).checked);
}

// A typed number; one that is not a number puts the field back.
function num(e: Event, use: (v: number) => void): void {
  const input = e.target as HTMLInputElement;
  const v = Number(input.value);
  if (input.value.trim() === '' || !Number.isFinite(v)) {
    input.value = input.defaultValue;
    return;
  }
  use(v);
}

function setStep(i: number, at: 0 | 1, v: Json): void {
  const next = steps.value.map(s => [...s] as [number, Json]);
  (next[i] as Json[])[at] = v;
  param('steps', next as Json);
}

function addStep(): void {
  const last = steps.value.at(-1) ?? [1, props.range.type === 'bool' ? false : 0];
  param('steps', [...steps.value, [...last]] as Json);
}

function removeStep(i: number): void {
  param('steps', steps.value.filter((_, j) => j !== i) as Json);
}
</script>

<style scoped>
.fields {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 8px 16px;
}

.fields .fields {
  padding: 4px 10px;
  border-left: 2px solid var(--line);
}

.caption {
  font-weight: 600;
  margin-right: 6px;
}

label {
  white-space: nowrap;
}

label input[type=number] {
  margin-left: 4px;
  width: 6.5em;
}

.steps {
  display: flex;
  flex-wrap: wrap;
  gap: 6px 14px;
  margin: 0;
  padding-left: 18px;
}

.steps input[type=number] {
  width: 5.5em;
}

.small {
  padding: 0 7px;
}
</style>
