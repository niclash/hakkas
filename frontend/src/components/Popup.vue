<script setup lang="ts">

defineProps({
  onOk: {
    type: Function,
    required: true
  },
  onCancel: {
    type: Function,
    required: true
  }
})
defineEmits({})
</script>

<template>
  <div class="hk-overlay">
    <div class="hk-dialog" role="dialog">
      <slot/>
      <div class="hk-actions">
        <button class="hk-button" @click="onCancel()">Cancel</button>
        <button class="hk-button primary" @click="onOk()">OK</button>
      </div>
    </div>
  </div>
</template>
