<template>
  <!-- A process port (06 §4.8): its public name, at the process's edge, joined to its block port. -->
  <div class="hk-port-node" :class="data.direction" data-testid="port-node" :data-direction="data.direction"
       :data-name="data.name" :data-target="target" :title="title">
    <Ref v-if="data.direction === 'out'" class="hk-port-socket" :emit="emit"
         :data="{type: 'socket', side: 'input', key: 'port', nodeId: data.id, payload: data.inputs.port?.socket}"/>
    <span class="hk-port-arrow" aria-hidden="true">{{ data.direction === 'in' ? '→' : '←' }}</span>
    <span class="hk-port-name" data-testid="port-name">{{ data.name }}</span>
    <Ref v-if="data.direction === 'in'" class="hk-port-socket" :emit="emit"
         :data="{type: 'socket', side: 'output', key: 'port', nodeId: data.id, payload: data.outputs.port?.socket}"/>
  </div>
</template>

<script lang="ts" setup>
import {computed} from "vue";
import {Ref} from "rete-vue-plugin";
import {PortNode} from "@/components/model";
import {editor} from "./viewmodel/default";

const props = defineProps<{ data: PortNode, emit: (...args: any[]) => void, seed?: number }>();

const target = computed(() => {
  void props.seed;
  return (editor.getNode(props.data.block)?.label ?? "") + "." + props.data.port;
});
const title = computed(() =>
    (props.data.direction === "in" ? "Process input " : "Process output ") + props.data.name + ", " + target.value);
</script>
