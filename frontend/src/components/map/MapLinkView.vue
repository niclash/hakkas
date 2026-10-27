<template>
  <!-- A link between processes (06 §4.17). -->
  <svg class="hk-map-link" data-testid="map-link" :data-link="name">
    <path :d="path"/>
  </svg>
</template>

<script lang="ts" setup>
import {computed} from "vue";

const props = defineProps<{
  data: { source: string, target: string, sourceOutput: string, targetInput: string },
  start?: unknown, end?: unknown, path: string
}>();

// "<process>.<outport> > <process>.<inport>", for the tests; a node's id is "process:<id>".
const graph = (id: string) => id.replace(/^process:/, "");
const name = computed(() => `${graph(props.data.source)}.${props.data.sourceOutput}>${graph(props.data.target)}.${props.data.targetInput}`);
</script>
