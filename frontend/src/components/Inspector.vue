<template>
  <aside class="hk-panel hk-inspector" data-testid="inspector" aria-label="Inspector">
    <div class="hk-panel-header">
      <h2 class="hk-panel-title">Inspector</h2>
    </div>

    <p v-if="view.kind === 'none'" class="hk-empty" data-testid="inspector-empty">
      Select a block to see its type and ports.
    </p>

    <template v-else-if="view.kind === 'one'">
      <h3 class="hk-inspector-id" data-testid="inspector-id">{{ view.id }}</h3>
      <div class="hk-inspector-type" data-testid="inspector-type">{{ view.component }}</div>
      <p v-if="!view.known" class="hk-inspector-note" data-testid="inspector-unknown">
        This block type is not registered: the block keeps its connections but does nothing.
      </p>
      <p v-else-if="view.description" class="hk-inspector-description" data-testid="inspector-description">
        {{ view.description }}
      </p>
      <section v-for="group in portGroups" :key="group.title" class="hk-inspector-ports"
               :data-testid="'inspector-' + group.key">
        <h4 class="hk-inspector-heading">{{ group.title }}</h4>
        <dl>
          <div v-for="port in group.ports" :key="port.id" class="hk-inspector-port" data-testid="inspector-port">
            <dt>
              <span class="hk-inspector-port-id" data-testid="port-id">{{ port.id }}</span>
              <span class="hk-inspector-port-type" data-testid="port-type">{{ port.type }}</span>
            </dt>
            <dd v-if="port.description" data-testid="port-description">{{ port.description }}</dd>
            <dd v-if="port.default !== undefined" class="hk-inspector-default" data-testid="port-default">
              Default: {{ port.default }}
            </dd>
          </div>
        </dl>
      </section>
      <div class="hk-actions">
        <button type="button" class="hk-button" data-testid="inspector-delete" @click="removeSelected">Delete</button>
      </div>
    </template>

    <template v-else>
      <h3 class="hk-inspector-id" data-testid="inspector-count">{{ view.ids.length }} blocks selected</h3>
      <p class="hk-inspector-list" data-testid="inspector-ids">{{ view.ids.join(", ") }}</p>
      <div class="hk-actions">
        <button type="button" class="hk-button" data-testid="inspector-delete" @click="removeSelected">
          Delete {{ view.ids.length }} blocks
        </button>
      </div>
    </template>
  </aside>
</template>

<script lang="ts" setup>
import {computed} from "vue";
import {vueModel} from "@/components/model";
import {inspect, PortView} from "@/components/inspector";
import {ComponentEvent} from "@/components/protocols/component";
import {editor} from "@/components/floweditor/viewmodel/default";

// The selected blocks (06 §4.2): the view's own state, so nothing here changes the process
// except through the engine's commands.
const view = computed(() => {
  const types: Record<string, ComponentEvent> = {};
  vueModel.components.value.forEach(c => types[c.name] = c as ComponentEvent);
  return inspect(vueModel.selectedBlocks.value, types);
});

const portGroups = computed(() => {
  const v = view.value;
  if (v.kind !== "one")
    return [];
  const groups: { key: string, title: string, ports: PortView[] }[] = [];
  if (v.inputs.length > 0)
    groups.push({key: "inputs", title: "Inputs", ports: v.inputs});
  if (v.outputs.length > 0)
    groups.push({key: "outputs", title: "Outputs", ports: v.outputs});
  return groups;
});

const removeSelected = () => editor.blockSelection?.removeSelected();
</script>
