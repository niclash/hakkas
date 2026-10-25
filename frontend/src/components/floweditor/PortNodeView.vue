<template>
  <!-- A process port (06 §4.8): its public name, at the process's edge, joined to its block port;
       with the environment channels it is bound to at its outer end (06 §4.14). -->
  <div class="hk-port-node" :class="[data.direction, channels.length > 0 ? 'bound' : '']" data-testid="port-node"
       :data-direction="data.direction" :data-name="data.name" :data-target="target" :data-bound="channels.join(', ')"
       :title="title">
    <span v-if="channels.length > 0 && data.direction === 'in'" class="hk-port-binding" data-testid="port-binding">⇄ {{ shown }}</span>
    <Ref v-if="data.direction === 'out'" class="hk-port-socket" :emit="emit"
         :data="{type: 'socket', side: 'input', key: 'port', nodeId: data.id, payload: data.inputs.port?.socket}"/>
    <span class="hk-port-arrow" aria-hidden="true">{{ data.direction === 'in' ? '→' : '←' }}</span>
    <span class="hk-port-name" data-testid="port-name">{{ data.name }}</span>
    <Ref v-if="data.direction === 'in'" class="hk-port-socket" :emit="emit"
         :data="{type: 'socket', side: 'output', key: 'port', nodeId: data.id, payload: data.outputs.port?.socket}"/>
    <span v-if="channels.length > 0 && data.direction === 'out'" class="hk-port-binding" data-testid="port-binding">⇄ {{ shown }}</span>
  </div>
</template>

<script lang="ts" setup>
import {computed} from "vue";
import {Ref} from "rete-vue-plugin";
import {PortNode, vueModel} from "@/components/model";
import {bindingLabel, bindingsAt} from "@/components/environment";
import {editor} from "./viewmodel/default";

const props = defineProps<{ data: PortNode, emit: (...args: any[]) => void, seed?: number }>();

const target = computed(() => {
  void props.seed;
  return (editor.getNode(props.data.block)?.label ?? "") + "." + props.data.port;
});
// The channels the port is bound to: a sensor feeding an inport, the actuators an outport drives.
const channels = computed(() => {
  void props.seed;
  return bindingsAt(vueModel.currentGraphId.value, props.data.direction, props.data.name).map(bindingLabel);
});
// Several actuators: the first and how many more (all of them in the tooltip).
const shown = computed(() => channels.value.length <= 1 ? channels.value.join("")
    : `${channels.value[0]} +${channels.value.length - 1}`);
const title = computed(() => (props.data.direction === "in" ? "Process input " : "Process output ") + props.data.name
    + ", " + target.value + (channels.value.length === 0 ? ""
        : (props.data.direction === "in" ? "; from the environment: " : "; to the environment: ") + channels.value.join(", ")));
</script>
