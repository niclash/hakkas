<script setup lang="ts">

// A modal dialog. `cancelLabel: null` shows only the OK button.
defineProps({
  onOk: {
    type: Function,
    required: true
  },
  onCancel: {
    type: Function,
    required: true
  },
  okLabel: {
    type: String,
    default: "OK"
  },
  cancelLabel: {
    type: [String, null],
    default: "Cancel"
  },
  danger: {
    type: Boolean,
    default: false
  }
})
defineEmits({})
</script>

<template>
  <div class="hk-overlay">
    <div class="hk-dialog" role="dialog" aria-modal="true" @keydown.escape="onCancel()">
      <slot/>
      <div class="hk-actions">
        <button v-if="cancelLabel !== null" class="hk-button" data-testid="dialog-cancel" @click="onCancel()">{{ cancelLabel }}</button>
        <button class="hk-button primary" :class="{danger}" data-testid="dialog-ok" @click="onOk()">{{ okLabel }}</button>
      </div>
    </div>
  </div>
</template>
