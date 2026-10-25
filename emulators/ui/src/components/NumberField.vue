<template>
  <input type="number" :min="min" :max="max" :step="step" :value="text" @focus="focused = true"
         @blur="blur" @input="text = ($event.target as HTMLInputElement).value" @change="change"
         @keydown.esc="revert">
</template>

<script setup lang="ts">
import {ref, watch} from 'vue';

// A number entry that follows `value` except while the user edits it: a row re-renders at
// every state update (a noisy input's value changes at each scan turn), and Vue sets an
// input's value again at each render, which would undo what is being typed. A typed number is
// committed on Enter or when the field is left; anything else is put back. Once the commit is
// answered — the emulator sends the new state before its answer (01 §2) — a field that is no
// longer being edited shows the emulator's value again, also after a refusal.
const props = defineProps<{value: number | null | undefined, commit: (v: number) => Promise<unknown>,
  min?: number, max?: number, step?: number}>();

const show = (v: number | null | undefined) => v === null || v === undefined ? '' : String(v);
const text = ref(show(props.value));
const focused = ref(false);
let pending = 0;

watch(() => props.value, v => {
  if (!focused.value)
    text.value = show(v);
});

function change(): void {
  const v = Number(text.value);
  if (text.value.trim() === '' || !Number.isFinite(v)) {
    revert();
    return;
  }
  if (v === props.value)
    return;
  pending++;
  void props.commit(v).finally(() => {
    pending--;
    if (!focused.value && pending === 0)
      revert();
  });
}

function revert(): void {
  text.value = show(props.value);
}

function blur(): void {
  focused.value = false;
  if (pending === 0)
    revert();
}
</script>
