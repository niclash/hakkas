<template>
  <!-- A process on the map (06 §4.17): its name and running mark, its inports on the left and
       its outports on the right, each with the channel it is bound to. -->
  <div class="hk-map-node" :class="{running: graph?.running}" data-testid="map-process" :data-graph="data.graph"
       :title="graph?.description || undefined" @dblclick.stop="openProcess(data.graph)">
    <div class="hk-map-title">
      <span class="hk-state" :class="{running: graph?.running}" data-testid="map-process-state"
            :title="graph?.running ? 'Running' : 'Stopped'"></span>
      <span class="hk-map-name" data-testid="map-process-name">{{ graph?.name ?? data.graph }}</span>
    </div>
    <div v-if="rows > 0" class="hk-map-ports">
      <div class="hk-map-column">
        <div v-for="name in data.inports" :key="'in:' + name" class="hk-map-port in" data-testid="map-inport"
             :data-port="name">
          <Ref class="hk-map-socket-ref" :emit="emit"
               :data="{type: 'socket', side: 'input', key: name, nodeId: data.id, payload: data.inputs[name]?.socket}"/>
          <span class="hk-map-port-name">{{ name }}</span>
          <span v-if="channel('in', name)" class="hk-map-channel" data-testid="map-channel">⇄ {{ channel('in', name) }}</span>
        </div>
      </div>
      <div class="hk-map-column out">
        <div v-for="name in data.outports" :key="'out:' + name" class="hk-map-port out" data-testid="map-outport"
             :data-port="name">
          <span v-if="channel('out', name)" class="hk-map-channel" data-testid="map-channel">⇄ {{ channel('out', name) }}</span>
          <span class="hk-map-port-name">{{ name }}</span>
          <Ref class="hk-map-socket-ref" :emit="emit"
               :data="{type: 'socket', side: 'output', key: name, nodeId: data.id, payload: data.outputs[name]?.socket}"/>
        </div>
      </div>
    </div>
  </div>
</template>

<script lang="ts" setup>
import {computed} from "vue";
import {Ref} from "rete-vue-plugin";
import {vueModel} from "@/components/model";
import {bindingLabel, bindingsAt} from "@/components/environment";
import {openProcess} from "./map";
import type {ProcessNode} from "./nodes";

const props = defineProps<{ data: ProcessNode, emit: (...args: any[]) => void }>();

const graph = computed(() => vueModel.graphs.value.get(props.data.graph));
const rows = computed(() => Math.max(props.data.inports.length, props.data.outports.length));
// The channels a port is bound to: the first, and how many more.
const channel = (direction: "in" | "out", name: string) => {
  const all = bindingsAt(props.data.graph, direction, name).map(bindingLabel);
  return all.length <= 1 ? all.join("") : `${all[0]} +${all.length - 1}`;
};
</script>
