<template>
  <!-- The device as a picture (control protocol §3.4, Colibri spec §8.4): the board with each part
       in its place and each channel at its point — an input operable where it is, an output shown
       as its lamp or value. Every channel stays in the signal panel below. -->
  <section v-if="size" class="board" data-testid="board">
    <div class="frame">
      <svg :viewBox="`0 0 ${W} ${H}`" role="img" :aria-label="'The device'">
        <image :href="board!.asset" x="0" y="0" :width="W" :height="H" preserveAspectRatio="none"/>
        <g v-for="e in board!.extras ?? []" :key="e.name" class="extra" data-testid="board-extra">
          <title>{{ e.name }}</title>
          <image v-if="e.asset" :href="e.asset" v-bind="box(e.rect)" preserveAspectRatio="none"/>
          <rect v-else class="empty" v-bind="box(e.rect)"/>
        </g>
        <g v-for="p in placed" :key="p.id" class="part" data-testid="board-part" :data-part="p.id">
          <title>{{ p.id }}: {{ p.name ?? p.type }}</title>
          <image v-if="p.asset" :href="p.asset" v-bind="box(p.rect!)" preserveAspectRatio="none"/>
          <template v-else>
            <rect class="empty" v-bind="box(p.rect!)"/>
            <text class="label" :x="cx(p.rect!)" :y="cy(p.rect!)"
                  :transform="`rotate(-90 ${cx(p.rect!)} ${cy(p.rect!)})`">{{ p.name ?? p.type }}</text>
          </template>
        </g>
        <g v-for="r in points" :key="r.row.key" class="point" :class="classes(r.row)" data-testid="board-point"
           :data-key="r.row.key" :data-value="text(r.row)" tabindex="0" role="button"
           @click="activate(r.row)" @keydown.enter.prevent="activate(r.row)"
           @pointerdown="press(r.row, true)" @pointerup="press(r.row, false)" @pointerleave="press(r.row, false)">
          <title>{{ tooltip(r.row) }}</title>
          <circle v-if="round(r.row)" :cx="r.x" :cy="r.y" r="7"/>
          <template v-else>
            <rect :x="r.x - width(r.row) / 2" :y="r.y - 8" :width="width(r.row)" height="16" rx="4"/>
            <text :x="r.x" :y="r.y + 3.5">{{ text(r.row) }}</text>
          </template>
        </g>
      </svg>
      <div v-if="editing" class="popover" data-testid="board-popover"
           :style="{left: `${100 * editing.x / W}%`, top: `${100 * editing.y / H}%`}" @keydown.escape="editing = undefined">
        <div class="head">
          <span class="mono">{{ editing.row.label }}</span>
          <button type="button" class="close" title="Close" aria-label="Close" @click="editing = undefined">×</button>
        </div>
        <template v-if="constant(editing.row)">
          <input v-if="slider(editing.row)" type="range" :min="editing.row.min" :max="editing.row.max"
                 :step="stepOf(editing.row)" :value="dragging ?? source(editing.row)" data-testid="board-slider"
                 @input="slide(editing.row, Number(($event.target as HTMLInputElement).value))"
                 @change="release(editing.row, Number(($event.target as HTMLInputElement).value))">
          <NumberField :value="source(editing.row) as number" :min="editing.row.min" :max="editing.row.max"
                       :step="stepOf(editing.row)" data-testid="board-number"
                       :commit="v => setSource(editing!.row, v)"/>
        </template>
        <span v-else class="summary">{{ describeSource(source(editing.row), editing.row.unit) }}</span>
        <button type="button" class="link" data-testid="board-row" @click="reveal(editing.row)">In the list…</button>
      </div>
    </div>
  </section>
</template>

<script setup lang="ts">
import {computed, ref, shallowRef, watch} from 'vue';
import type {Client} from '../protocol/client';
import type {Json, Rect, Row} from '../protocol/types';
import {describeSource, formatValue, kindOf, stepOf} from '../sources';
import NumberField from './NumberField.vue';

const props = defineProps<{client: Client}>();
const m = props.client.mirror;
const board = computed(() => m.scene.board);

// The picture's width in SVG units; its height follows the board's picture.
const W = 1000;
const size = shallowRef<{width: number, height: number} | undefined>();
const H = computed(() => size.value === undefined ? 0 : Math.round(W * size.value.height / size.value.width));

watch(() => board.value?.asset, asset => {
  size.value = undefined;
  if (asset === undefined || !m.scene.parts.some(p => p.rect !== undefined))
    return;
  const img = new Image();
  img.onload = () => {
    if (board.value?.asset === asset && img.naturalWidth > 0)
      size.value = {width: img.naturalWidth, height: img.naturalHeight};
  };
  img.src = asset;
}, {immediate: true});

const box = (r: Rect) => ({x: r.x * W, y: r.y * H.value, width: r.w * W, height: r.h * H.value});
const cx = (r: Rect) => (r.x + r.w / 2) * W;
const cy = (r: Rect) => (r.y + r.h / 2) * H.value;

// The filled parts with a place; an empty place shows the board's own slot.
const placed = computed(() => m.scene.parts.filter(p => p.rect !== undefined && p.type !== null));

// The rows with a point; several at one point are stacked downwards.
const points = computed(() => {
  const seen = new Map<string, number>();
  return m.scene.rows.filter(r => r.at !== undefined).map(row => {
    const at = `${row.at!.x},${row.at!.y}`;
    const n = seen.get(at) ?? 0;
    seen.set(at, n + 1);
    return {row, x: row.at!.x * W, y: row.at!.y * H.value + 18 * n};
  });
});

// ---- Values ----------------------------------------------------------------------------

const value = (r: Row) => m.values[r.key];
const source = (r: Row) => m.values[`${r.key}.source`];
const constant = (r: Row) => r.kind === 'input' && kindOf(source(r)) === 'constant';
const slider = (r: Row) => r.control !== 'number' && r.min !== undefined && r.max !== undefined;
const round = (r: Row) => r.type === 'bool';
// On the picture room is short: three significant digits (whole numbers from 1000 on), no unit;
// the list and the tooltip have the value as it is.
const text = (r: Row) => {
  const v = value(r);
  if (v === null || v === undefined)
    return '–';
  if (typeof v !== 'number')
    return v === true ? 'on' : v === false ? 'off' : String(v);
  return Math.abs(v) >= 1000 ? String(Math.round(v)) : String(Number(v.toPrecision(3)));
};
const width = (r: Row) => 6 + 6 * text(r).length;
const classes = (r: Row) => [r.kind, value(r) === true ? 'on' : value(r) === false ? 'off' : '',
  value(r) === null || value(r) === undefined ? 'none' : '', constant(r) && r.type === 'bool' ? 'operable' : ''];
const tooltip = (r: Row) => [`${r.label} (${r.kind})`, r.description,
  value(r) === null || value(r) === undefined ? 'no value yet' : formatValue(value(r), r) + (r.unit ? ' ' + r.unit : ''),
  r.kind === 'input' ? 'source: ' + describeSource(source(r), r.unit) : ''].filter(s => s).join('\n');

// Sends a `set`; a refusal is shown as a notice. Settles when it is answered.
function set(key: string, v: Json): Promise<unknown> {
  return props.client.request('set', {key, value: v}).catch((e: Error) =>
    props.client.notify({level: 'error', message: e.message}));
}

const setSource = (r: Row, v: Json) => set(`${r.key}.source`, v);

// ---- Operating it ----------------------------------------------------------------------

const editing = ref<{row: Row, x: number, y: number} | undefined>();

// A toggle switches where it is; a momentary button is held (press); a number input opens its
// slider; anything else shows the row in the list.
function activate(r: Row): void {
  if (constant(r) && r.type === 'bool') {
    if (r.control !== 'button')
      setSource(r, source(r) !== true);
    return;
  }
  if (r.kind === 'input') {
    const p = points.value.find(q => q.row.key === r.key)!;
    editing.value = editing.value?.row.key === r.key ? undefined : {row: r, x: p.x, y: p.y};
    return;
  }
  reveal(r);
}

let held: string | undefined;
function press(r: Row, down: boolean): void {
  if (!constant(r) || r.type !== 'bool' || r.control !== 'button')
    return;
  if (down) {
    held = r.key;
    setSource(r, true);
  } else if (held === r.key) {
    held = undefined;
    setSource(r, false);
  }
}

// The row in the signal panel, scrolled to and marked for a moment.
function reveal(r: Row): void {
  editing.value = undefined;
  const el = document.querySelector<HTMLElement>(`[data-testid=row][data-key="${r.key}"]`);
  if (el === null)
    return;
  el.scrollIntoView({behavior: 'smooth', block: 'center'});
  el.classList.add('flash');
  setTimeout(() => el.classList.remove('flash'), 1500);
}

// A slider sends while it is dragged, at most every 50 ms, and its last position when let go.
const dragging = ref<number | undefined>();
let lastSent = 0;
let trailing: ReturnType<typeof setTimeout> | undefined;
function slide(r: Row, v: number): void {
  dragging.value = v;
  clearTimeout(trailing);
  const wait = lastSent + 50 - Date.now();
  if (wait <= 0) {
    lastSent = Date.now();
    setSource(r, v);
  } else {
    trailing = setTimeout(() => slide(r, v), wait);
  }
}

function release(r: Row, v: number): void {
  clearTimeout(trailing);
  dragging.value = undefined;
  setSource(r, v);
}
</script>

<style scoped>
.board {
  background: var(--panel);
  border: 1px solid var(--line);
  border-radius: var(--radius);
  padding: 8px;
}

.frame {
  position: relative;
}

svg {
  display: block;
  width: 100%;
  height: auto;
  user-select: none;
}

.empty {
  fill: var(--bg);
  stroke: var(--line);
  stroke-dasharray: 4 3;
}

.label {
  font-size: 14px;
  fill: var(--muted);
  text-anchor: middle;
  dominant-baseline: middle;
}

.point {
  cursor: pointer;
  outline: none;
}

.point circle, .point rect {
  fill: var(--panel);
  stroke-width: 2;
  stroke: var(--accent);
}

.point.output circle, .point.output rect {
  stroke: var(--ok);
}

.point.measured circle, .point.measured rect {
  stroke: var(--muted);
}

.point.on circle {
  fill: var(--lamp-on);
}

.point.none circle, .point.none rect {
  stroke-dasharray: 3 2;
  opacity: 0.75;
}

.point:hover circle, .point:hover rect, .point:focus-visible circle, .point:focus-visible rect {
  stroke-width: 3;
}

.point text {
  font-size: 10px;
  font-family: var(--mono);
  text-anchor: middle;
  fill: var(--text);
  pointer-events: none;
}

.popover {
  position: absolute;
  transform: translate(-50%, 14px);
  z-index: 3;
  display: flex;
  flex-direction: column;
  gap: 6px;
  min-width: 200px;
  padding: 8px 10px;
  background: var(--panel);
  border: 1px solid var(--line);
  border-radius: var(--radius);
  box-shadow: 0 6px 20px rgba(0, 0, 0, 0.2);
}

.head {
  display: flex;
  justify-content: space-between;
  align-items: center;
}

.mono {
  font-family: var(--mono);
}

.close {
  border: none;
  background: none;
  font-size: 16px;
  cursor: pointer;
  color: var(--muted);
}

.summary {
  font-size: 13px;
  color: var(--muted);
}

.link {
  align-self: flex-start;
  border: none;
  background: none;
  padding: 0;
  color: var(--accent);
  cursor: pointer;
}
</style>
