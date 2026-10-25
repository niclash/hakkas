<template>
  <form class="editor" data-testid="source-editor" @submit.prevent="emit('apply', draft)" @keydown.esc="emit('cancel')">
    <SourceFields v-model="draft" :kinds="kinds" :range="row" :unit="row.unit"/>
    <div class="actions">
      <button type="button" data-testid="cancel" @click="emit('cancel')">Cancel</button>
      <button type="submit" class="primary" data-testid="apply">Apply</button>
    </div>
  </form>
</template>

<script setup lang="ts">
import {computed, shallowRef} from 'vue';
import type {Json, Row, SourceKind} from '../protocol/types';
import {newSource} from '../sources';
import SourceFields from './SourceFields.vue';

const props = defineProps<{row: Row, source: Json | undefined}>();
const emit = defineEmits<{apply: [Json], cancel: []}>();

const kinds = computed((): SourceKind[] => props.row.sources ?? (props.row.kind === 'signal' ? ['pulse'] : ['constant']));
// A draft, edited here and sent only by Apply; the row shows the emulator's source meanwhile.
// (A copy through JSON: the mirror's objects are reactive proxies, which structuredClone refuses.)
const draft = shallowRef<Json>(props.source !== undefined ? JSON.parse(JSON.stringify(props.source)) as Json
  : newSource(kinds.value[0]!, props.row));
</script>

<style scoped>
.editor {
  display: flex;
  align-items: flex-start;
  gap: 16px;
  padding: 6px 0;
}

.editor > :first-child {
  flex: 1;
}

.actions {
  display: flex;
  gap: 8px;
}
</style>
