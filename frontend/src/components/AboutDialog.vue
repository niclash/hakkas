<template>
  <Popup :onOk="() => emit('close')" :onCancel="() => emit('close')" :cancelLabel="null" okLabel="Close">
    <h4>About</h4>
    <dl class="hk-about" data-testid="about">
      <dt>Engine</dt><dd data-testid="about-label">{{ str("label") || "—" }}</dd>
      <dt>Version</dt><dd data-testid="about-version">{{ str("version") || "—" }}</dd>
      <dt>Engine id</dt><dd>{{ str("id") || "—" }}</dd>
      <dt>Logged in as</dt><dd data-testid="about-user">{{ vueModel.user.value }} ({{ vueModel.role.value }})</dd>
      <dt>May</dt><dd data-testid="about-capabilities">{{ vueModel.capabilities.value.join(", ") || "—" }}</dd>
    </dl>
  </Popup>
</template>

<script lang="ts" setup>
import Popup from "@/components/Popup.vue";
import {vueModel} from "@/components/model";

// The engine's runtime:runtime (02 §3.2) and the session (06 §4.12).
const emit = defineEmits<{ close: [] }>();
const str = (key: string) => {
  const v = vueModel.runtime.value?.[key];
  return typeof v === "string" ? v : "";
};
</script>
