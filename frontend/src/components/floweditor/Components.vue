<template>
  <section ref="panel" class="hk-panel">
    <div class="hk-panel-header">
      <h2 class="hk-panel-title">Block types</h2>
    </div>
    <input class="hk-input hk-palette-search" type="search" data-testid="palette-search" v-model="search"
           placeholder="Search" aria-label="Search block types" spellcheck="false" autocomplete="off"
           @keydown.escape="search = ''"/>
    <details class="hk-group" open v-for="(entries, family) in groups" :key="family" :data-family="family">
      <summary>{{ family }}</summary>
      <div v-for="entry in entries" :key="entry.name" class="hk-palette-row">
        <button class="hk-palette-item" draggable="true"
                data-testid="block-type" :data-type="entry.name" :title="entry.tooltip"
                @click="emit('onSelection', entry.name)" @dragstart="dragStart($event, entry.name)">
          {{ entry.short }}
        </button>
        <button v-if="entry.subgraph" type="button" class="hk-icon-button hk-palette-open" data-testid="open-type"
                :data-type="entry.name" :aria-label="'Open block type ' + entry.name" title="Open the type's inside"
                @click="emit('openType', entry.name)">↗</button>
      </div>
    </details>
    <p v-if="Object.keys(groups).length === 0" class="hk-empty" data-testid="palette-none">No block type matches</p>
  </section>
</template>

<script lang="ts" setup>
import {computed, nextTick, ref, watch} from "vue";
import {vueModel} from '../model';
import {BLOCK_TYPE_MIME, paletteGroups} from "@/components/palette";

// The block types from the session's component:component events (model.ts attachModel), by
// family (06 §4.5). A click adds one in the middle of the view; dragging one onto the editor
// adds it where it is dropped.
const search = ref("");
const groups = computed(() => paletteGroups(vueModel.components.value, search.value));
const panel = ref<HTMLElement | null>(null);

// A search opens every family with a match, also one that was closed.
watch(search, async () => {
  await nextTick();
  panel.value?.querySelectorAll("details").forEach(d => d.open = true);
});

const dragStart = (event: DragEvent, type: string) => {
  event.dataTransfer?.setData(BLOCK_TYPE_MIME, type);
  event.dataTransfer?.setData("text/plain", type);
  if (event.dataTransfer)
    event.dataTransfer.effectAllowed = "copy";
};

const emit = defineEmits<{
  onSelection: [value: string],
  openType: [type: string],
}>();
</script>
