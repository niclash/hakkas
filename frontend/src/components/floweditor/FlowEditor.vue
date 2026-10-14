<script lang="ts" setup>
import Processes from "@/components/Processes.vue";
import Components from "@/components/floweditor/Components.vue";
import {Connection as WsConnection, Connection, Packet} from "@/components/protocols/websocket";
import {onMounted, ref, watch} from "vue";
import ReteEditor from "@/components/floweditor/ReteController.vue";
import Inspector from "@/components/Inspector.vue";
import {can, vueModel} from "@/components/model";
import {editor as reteEditor} from "@/components/floweditor/viewmodel/default";
import {RuntimeEvent} from "@/components/protocols/runtime";

const name = "FlowEditor";
const components = {Components, Processes};
const callbackMethods: Record<string, Function[]> = {};

const props = defineProps({
  secret: {
    type: String,
    required: true
  }
});

const openProcess = (processId: string) => {
  console.log("openProcess(" + processId + ");");
  vueModel.connection.value?.proto.graph.request_connect(processId);
}

// A block picked in the palette goes in the middle of the view (06 §4.5).
const addComponent = (componentType: string) => {
  const newNodeName = vueModel.nextName(componentType, (name) => reteEditor.getNodes().some(n => n.label === name));
  const at = reteEditor.viewCenter();
  vueModel.connection.value?.proto.graph.request_addnode(newNodeName, componentType, Math.round(at.x - 90), Math.round(at.y - 60));
}

const comps = ref(null);
const procs = ref(null);
const editor = ref(null);

defineExpose({
  callbacks: []
});

onMounted(() => {
  console.log("FlowEditor.mounted()");
});


</script>


<template>
  <div class="hk-workspace">
    <aside class="hk-sidebar">
      <Processes ref="procs" :current="vueModel.currentGraphId.value" @onSelection="(processName:string) => {openProcess(processName)}"/>
      <Components v-if="can.edit" ref="comps" @onSelection="(compType:string) => {addComponent(compType)}"
                  @openType="(type: string) => openProcess(type)"/>
    </aside>
    <ReteEditor ref="editor"/>
    <Inspector/>
  </div>
</template>

<style scoped>

</style>