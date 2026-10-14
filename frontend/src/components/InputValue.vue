<template>
  <div class="hk-inspector-value" :data-testid="'input-value-' + port.id">
    <p v-if="source.kind === 'connected'" class="hk-inspector-source" data-testid="input-state">
      Connected from {{ source.from }}
    </p>
    <template v-else>
      <div class="hk-inspector-value-row">
        <label v-if="editor === 'bool'" class="hk-switch">
          <input type="checkbox" role="switch" data-testid="value-switch" :checked="shownBool"
                 :aria-label="port.id + ' value'" @change="sendBool(($event.target as HTMLInputElement).checked)"/>
          <span>{{ shownBool ? "true" : "false" }}</span>
        </label>
        <input v-else ref="field" class="hk-input" data-testid="value-field" v-model="draft"
               :inputmode="editor === 'number' ? 'decimal' : 'text'" spellcheck="false" autocomplete="off"
               :placeholder="resting" :aria-label="port.id + ' value'"
               :aria-invalid="error !== ''" @focus="editing = true" @blur="leave"
               @keydown.enter.prevent="send" @keydown.escape.stop.prevent="revert"/>
        <button v-if="source.kind === 'fixed'" type="button" class="hk-icon-button" data-testid="value-clear"
                :aria-label="'Use the default of ' + port.id" title="Use the default" @click="clear">&times;</button>
      </div>
      <p class="hk-inspector-source" data-testid="input-state">
        {{ source.kind === 'fixed' ? 'Fixed value' : 'Default: ' + (resting === '' ? 'empty' : resting) }}
      </p>
    </template>
    <p v-if="error" class="hk-inspector-note" role="alert" data-testid="value-error">{{ error }}</p>
  </div>
</template>

<script lang="ts" setup>
import {computed, onBeforeUnmount, onMounted, onUnmounted, ref, watch} from "vue";
import {errorClaims, vueModel} from "@/components/model";
import {PortView} from "@/components/inspector";
import {initialRefusals, InputSource, parseTyped, restingValue, sameValue, valueEditor} from "@/components/initials";

// One input of the selected block in the inspector (06 §4.2): what feeds it, and its fixed
// value's editor. The value changes only with the engine's graph:addinitial/removeinitial.
const props = defineProps<{ block: string, port: PortView }>();

const source = computed<InputSource>(() => props.port.source ?? {kind: "default"});
const editor = computed(() => valueEditor(props.port.type));
const fixed = computed(() => source.value.kind === "fixed" ? source.value.value : undefined);
const resting = computed(() => restingValue(props.port.type));
const shownBool = computed(() => (fixed.value ?? resting.value) === "true");

const field = ref<HTMLInputElement | null>(null);
const draft = ref(fixed.value ?? "");
const editing = ref(false);
const error = ref("");
// The refusals of the command sent and not answered yet.
let pending: string[] | undefined;

watch(source, () => {
  pending = undefined;
  if (!editing.value || draft.value === (fixed.value ?? "")) {
    draft.value = fixed.value ?? "";
    error.value = "";
  }
}, {deep: true});

const graph = () => vueModel.connection.value?.proto.graph;

const add = (value: number | string | boolean) => {
  error.value = "";
  pending = initialRefusals(props.block, props.port.id, "addinitial");
  graph()?.request_addinitial(value, props.block, props.port.id, -1, undefined);
};

const clear = () => {
  error.value = "";
  pending = initialRefusals(props.block, props.port.id, "removeinitial");
  graph()?.request_removeinitial(undefined, props.block, props.port.id, -1);
};

const sendBool = (checked: boolean) => add(checked);

const send = () => {
  const typed = parseTyped(editor.value === "string" ? "string" : "number", draft.value);
  if ("error" in typed) {
    error.value = typed.error;
    return;
  }
  if ("clear" in typed) {
    if (fixed.value !== undefined)
      clear();
    else
      error.value = "";
    return;
  }
  if (fixed.value !== undefined && sameValue(editor.value, fixed.value, typed.value)) {
    error.value = "";
    return;
  }
  add(typed.value);
};

// Leaving the field sends a changed value; after an error, it puts the value back.
const leave = () => {
  editing.value = false;
  if (error.value !== "")
    revert();
  else if (draft.value !== (fixed.value ?? ""))
    send();
};

const revert = () => {
  pending = undefined;
  draft.value = fixed.value ?? "";
  error.value = "";
  field.value?.blur();
};

const claim = (message: string): boolean => {
  if (pending === undefined || !pending.includes(message))
    return false;
  pending = undefined;
  error.value = message;
  return true;
};

// Clicking another block keeps the focus in the field (the editor takes the pointer) and
// replaces this input: a value being typed is sent first.
onBeforeUnmount(() => {
  if (editing.value && error.value === "" && draft.value !== (fixed.value ?? ""))
    send();
});

onMounted(() => errorClaims.push(claim));
onUnmounted(() => errorClaims.splice(errorClaims.indexOf(claim), 1));
</script>
