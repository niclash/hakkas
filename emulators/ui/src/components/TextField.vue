<template>
  <input type="text" :value="text" spellcheck="false" autocomplete="off" @focus="focused = true" @blur="blur"
         @input="text = ($event.target as HTMLInputElement).value" @change="change" @keydown.esc="revert">
</template>

<script setup lang="ts">
import {ref, watch} from 'vue';

// A text entry that follows `value` except while the user edits it (as NumberField does): the
// text is committed on Enter or when the field is left, and once the commit is answered a field
// that is no longer edited shows the emulator's value again, also after a refusal.
const props = defineProps<{value: string, commit: (v: string) => Promise<unknown>}>();

const text = ref(props.value);
const focused = ref(false);
let pending = 0;

watch(() => props.value, v => {
  if (!focused.value)
    text.value = v;
});

function change(): void {
  if (text.value === props.value)
    return;
  pending++;
  void props.commit(text.value).finally(() => {
    pending--;
    if (!focused.value && pending === 0)
      revert();
  });
}

function revert(): void {
  text.value = props.value;
}

function blur(): void {
  focused.value = false;
  if (pending === 0)
    revert();
}
</script>
