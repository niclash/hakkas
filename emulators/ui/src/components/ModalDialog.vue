<template>
  <div class="overlay" @pointerdown.self="emit('cancel')">
    <form class="dialog" role="dialog" aria-modal="true" :aria-label="title" data-testid="dialog"
          @submit.prevent="emit('ok')" @keydown.esc.prevent="emit('cancel')">
      <h3>{{ title }}</h3>
      <slot/>
      <p v-if="error" class="error" role="alert" data-testid="dialog-error">{{ error }}</p>
      <div class="actions">
        <slot name="actions">
          <button type="button" data-testid="dialog-cancel" @click="emit('cancel')">Cancel</button>
          <button type="submit" class="primary" :class="{danger}" data-testid="dialog-ok" :disabled="busy">{{ okLabel }}</button>
        </slot>
      </div>
    </form>
  </div>
</template>

<script setup lang="ts">
// A modal dialog: a title, its content, and Cancel and OK (or actions of its own).
defineProps<{title: string, okLabel?: string, error?: string, busy?: boolean, danger?: boolean}>();
const emit = defineEmits<{ok: [], cancel: []}>();
</script>

<style scoped>
.overlay {
  position: fixed;
  inset: 0;
  background: rgb(0 0 0 / 35%);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 10;
}

.dialog {
  background: var(--panel);
  border: 1px solid var(--line);
  border-radius: 8px;
  padding: 16px 20px;
  min-width: 360px;
  max-width: 520px;
  box-shadow: 0 10px 30px rgb(0 0 0 / 25%);
}

h3 {
  margin: 0 0 12px;
  font-size: 16px;
}

.error {
  color: var(--error);
  margin: 8px 0 0;
}

.actions {
  display: flex;
  justify-content: flex-end;
  gap: 8px;
  margin-top: 16px;
}

.danger {
  background: var(--error);
  border-color: var(--error);
}
</style>
