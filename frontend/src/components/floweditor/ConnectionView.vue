<template>
  <!-- A connection, or a value node's line (06 §4.3), with its input's live value (06 §4.7). -->
  <svg :class="kind === 'value' ? 'hk-value-connection' : kind === 'connection' ? 'hk-connection' : 'hk-port-connection ' + kind"
       :data-testid="kind === 'value' ? 'value-connection' : kind === 'port' ? 'port-connection'
           : kind === 'binding' ? 'binding-connection' : 'connection'">
    <path ref="line" :d="path"/>
    <foreignObject v-if="value !== undefined" :x="middle.x - 140" :y="middle.y - 24" width="280" height="48">
      <div class="hk-live" :class="{stale: value.stale}" data-testid="live-value" :data-target="target"
           :title="value.stale ? 'Not current: the connection to the engine was lost' : 'Live value'">
        <span>{{ text }}</span>
      </div>
    </foreignObject>
  </svg>
</template>

<script lang="ts" setup>
import {computed, nextTick, ref, watch} from "vue";
import {isBindingNode, isPortNode, isValueNode, PrimitiveNode} from "@/components/model";
import {editor} from "./viewmodel/default";
import {live, liveValue} from "./live";
import {formatLive} from "@/components/watches";

const props = defineProps<{
  data: { source: string, target: string, targetInput: string },
  start?: unknown, end?: unknown, path: string
}>();

// A connection, a value node's line (06 §4.3), a process port's line (06 §4.8) or a binding's
// (06 §4.14).
const kind = computed(() => {
  const source = editor.getNode(props.data.source), target = editor.getNode(props.data.target);
  if (isValueNode(source))
    return "value";
  if (isBindingNode(source) || isBindingNode(target))
    return "binding";
  return isPortNode(source) || isPortNode(target) ? "port" : "connection";
});
const block = computed(() => {
  void live.version;
  const node = editor.getNode(props.data.target);
  return node instanceof PrimitiveNode ? node : undefined;
});
const target = computed(() => {
  void live.version;
  return block.value ? block.value.label + "." + props.data.targetInput : "";
});
const value = computed(() => {
  void live.version;
  // A port's line shows no value: the input's own line does.
  return block.value && kind.value !== "port" && kind.value !== "binding" ? liveValue({node: block.value.label, port: props.data.targetInput}) : undefined;
});
const text = computed(() =>
    value.value && block.value ? formatLive(block.value.inputType(props.data.targetInput), value.value.data) : "");

// The middle of the drawn line, where the label goes.
const line = ref<SVGPathElement | null>(null);
const middle = ref({x: 0, y: 0});
watch(() => [props.path, value.value !== undefined], async () => {
  await nextTick();
  const path = line.value;
  if (path === null || props.path === "")
    return;
  const p = path.getPointAtLength(path.getTotalLength() / 2);
  middle.value = {x: p.x, y: p.y};
}, {immediate: true});
</script>
