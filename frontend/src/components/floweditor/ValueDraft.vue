<template>
  <div class="hk-value-draft" data-testid="value-draft" :style="{left: x + 'px', top: y + 'px'}"
       @pointerdown.stop>
    <div v-if="kind === 'bool'" class="hk-value-draft-row" role="group" :aria-label="port + ' value'">
      <button ref="first" type="button" class="hk-button" data-testid="value-draft-true" @click="add(true)">true</button>
      <button type="button" class="hk-button" data-testid="value-draft-false" @click="add(false)">false</button>
      <button type="button" class="hk-icon-button" aria-label="Cancel" @click="emit('close')">&times;</button>
    </div>
    <input v-else ref="first" class="hk-input" data-testid="value-draft-field" v-model="text"
           :inputmode="kind === 'number' ? 'decimal' : 'text'" spellcheck="false" autocomplete="off"
           :placeholder="kind === 'number' ? 'A number' : 'Text'" :aria-label="port + ' value'"
           :aria-invalid="error !== ''" @keydown.enter.prevent="send" @keydown.escape.stop.prevent="emit('close')"
           @blur="leave"/>
    <p v-if="error" class="hk-inspector-note" role="alert" data-testid="value-draft-error">{{ error }}</p>
  </div>
</template>

<script lang="ts" setup>
import {computed, onMounted, onUnmounted, ref} from "vue";
import {errorClaims, vueModel} from "@/components/model";
import {initialRefusals, parseTyped, valueEditor} from "@/components/initials";

// The value asked for when a line drawn from a free input is dropped on the canvas (06 §4.3):
// beside the input (x, y: the input's socket in the editor pane). The parent closes it when
// the engine's graph:addinitial for the input arrives.
const props = defineProps<{ block: string, port: string, portType: string, x: number, y: number }>();
const emit = defineEmits<{ close: [] }>();

const kind = computed(() => valueEditor(props.portType));
const text = ref("");
const error = ref("");
const first = ref<HTMLElement | null>(null);
let pending: string[] | undefined;

const add = (value: number | string | boolean) => {
  error.value = "";
  pending = initialRefusals(props.block, props.port, "addinitial");
  vueModel.connection.value?.proto.graph.request_addinitial(value, props.block, props.port, -1, undefined);
};

const send = () => {
  const typed = parseTyped(kind.value === "string" ? "string" : "number", text.value);
  if ("error" in typed)
    error.value = typed.error;
  else if ("clear" in typed)
    emit("close");
  else
    add(typed.value);
};

// Clicking elsewhere drops the box, unless a value was sent and is waiting for its event.
const leave = () => {
  if (pending === undefined)
    emit("close");
};

const claim = (message: string): boolean => {
  if (pending === undefined || !pending.includes(message))
    return false;
  pending = undefined;
  error.value = message;
  return true;
};

onMounted(() => {
  errorClaims.push(claim);
  first.value?.focus();
});
onUnmounted(() => errorClaims.splice(errorClaims.indexOf(claim), 1));
</script>
