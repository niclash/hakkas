<template>
  <!-- A connection, or a value node's line (06 §4.3), with its input's live value (06 §4.7). -->
  <svg :class="valueLine ? 'hk-value-connection' : 'hk-connection'"
       :data-testid="valueLine ? 'value-connection' : 'connection'">
    <path ref="line" :d="path"/>
    <foreignObject v-if="value !== undefined" :x="middle.x - 70" :y="middle.y - 12" width="140" height="24">
      <div class="hk-live" :class="{stale: value.stale}" data-testid="live-value" :data-target="target"
           :title="value.stale ? 'Not current: the connection to the engine was lost' : 'Live value'">
        <span>{{ text }}</span>
      </div>
    </foreignObject>
  </svg>
</template>

<script lang="ts" setup>
import {computed, nextTick, ref, watch} from "vue";
import {isValueNode, PrimitiveNode} from "@/components/model";
import {editor} from "./viewmodel/default";
import {live, liveValue} from "./live";
import {formatLive} from "@/components/watches";

const props = defineProps<{
  data: { source: string, target: string, targetInput: string },
  start?: unknown, end?: unknown, path: string
}>();

const valueLine = computed(() => isValueNode(editor.getNode(props.data.source)));
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
  return block.value ? liveValue({node: block.value.label, port: props.data.targetInput}) : undefined;
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
