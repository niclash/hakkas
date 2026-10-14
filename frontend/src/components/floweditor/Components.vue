<template>
  <section class="hk-panel">
    <div class="hk-panel-header">
      <h2 class="hk-panel-title">Block types</h2>
    </div>
    <details class="hk-group" open v-for="(components, section) in groupComponents()" :key="section">
      <summary>{{ section }}</summary>
      <button v-for="component in components" :key="component.fullId" class="hk-palette-item"
              data-testid="block-type" :data-type="component.fullId" :title="component.description"
              @click="emit('onSelection', component.fullId)">
        {{ component.name }}
      </button>
    </details>
  </section>
</template>

<script lang="ts" setup>
import {ComponentEvent} from "@/components/protocols/component";
import {vueModel} from '../model';

// The block types come from the session's component:component events (model.ts attachModel).
const groupComponents = () => {
  const groups: Record<string, ComponentEvent[]> = {};
  vueModel.components.value.forEach(component => {
    const slash = component.name.indexOf('/');
    const section = slash < 0 ? "" : component.name.substring(0, slash);
    const name = component.name.substring(slash + 1);
    if (!groups[section]) {
      groups[section] = [];
    }
    groups[section].push({...component, name: name, fullId: component.name});
  });
  return groups;
};

const emit = defineEmits<{
  onSelection: [value: string]
}>();

</script>
