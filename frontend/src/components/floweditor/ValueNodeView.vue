<template>
  <div class="hk-value-node" data-testid="value-node" :data-target="data.blockLabel + '.' + data.port"
       :class="{editing}" :title="editing ? '' : 'Double-click to change'" @dblclick.stop="edit">
    <input v-if="editing" ref="field" class="hk-value-field" data-testid="value-node-field" v-model="draft"
           :inputmode="kind === 'number' ? 'decimal' : 'text'" spellcheck="false" autocomplete="off"
           :aria-label="data.port + ' value'" :aria-invalid="error !== ''"
           @pointerdown.stop @dblclick.stop @keydown.enter.prevent="send" @keydown.escape.stop.prevent="cancel"
           @keydown.stop @blur="leave"/>
    <span v-else class="hk-value-text" data-testid="value-text">{{ data.value }}</span>
    <Ref class="hk-value-socket" :emit="emit"
         :data="{type: 'socket', side: 'output', key: 'value', nodeId: data.id, payload: data.outputs.value?.socket}"/>
    <div v-if="error" class="hk-value-error" role="alert" data-testid="value-node-error">{{ error }}</div>
  </div>
</template>

<script lang="ts" setup>
import {computed, nextTick, onMounted, onUnmounted, ref} from "vue";
import {Ref} from "rete-vue-plugin";
import {errorClaims, ValueNode, vueModel} from "@/components/model";
import {initialRefusals, parseTyped, sameValue, valueEditor} from "@/components/initials";

// A fixed value drawn beside its input (06 §4.3). Double-click changes it: a field for a number
// or text, a toggle for a bool. The value shown changes only with the engine's event.
const props = defineProps<{ data: ValueNode, emit: (...args: any[]) => void, seed?: number }>();

const kind = computed(() => valueEditor(props.data.portType));
const editing = ref(false);
const draft = ref("");
const error = ref("");
const field = ref<HTMLInputElement | null>(null);
let pending: string[] | undefined;

const add = (value: number | string | boolean) => {
  error.value = "";
  pending = initialRefusals(props.data.blockLabel, props.data.port, "addinitial");
  vueModel.connection.value?.proto.graph.request_addinitial(value, props.data.blockLabel, props.data.port, -1, undefined);
};

const edit = async () => {
  if (kind.value === "bool") {
    add(props.data.value !== "true");
    return;
  }
  draft.value = props.data.value;
  error.value = "";
  editing.value = true;
  await nextTick();
  field.value?.focus();
  field.value?.select();
};

const send = () => {
  const typed = parseTyped(kind.value === "string" ? "string" : "number", draft.value);
  if ("error" in typed) {
    error.value = typed.error;
    return;
  }
  editing.value = false;
  if ("clear" in typed) {
    pending = initialRefusals(props.data.blockLabel, props.data.port, "removeinitial");
    vueModel.connection.value?.proto.graph.request_removeinitial(undefined, props.data.blockLabel, props.data.port, -1);
    return;
  }
  if (!sameValue(kind.value, props.data.value, typed.value))
    add(typed.value);
};

const cancel = () => {
  editing.value = false;
  error.value = "";
};

const leave = () => {
  if (!editing.value)
    return;
  if (error.value !== "")
    cancel();
  else
    send();
};

const claim = (message: string): boolean => {
  if (pending === undefined || !pending.includes(message))
    return false;
  pending = undefined;
  error.value = message;
  return true;
};

onMounted(() => errorClaims.push(claim));
onUnmounted(() => errorClaims.splice(errorClaims.indexOf(claim), 1));
</script>
