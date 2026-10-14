<template>
  <!-- A value node's line to its input (06 §4.3), drawn apart from the process's connections. -->
  <svg class="hk-value-connection" data-testid="value-connection">
    <path :d="path"/>
  </svg>
</template>

<script lang="ts" setup>
defineProps<{ data: unknown, start?: unknown, end?: unknown, path: string }>();
</script>
