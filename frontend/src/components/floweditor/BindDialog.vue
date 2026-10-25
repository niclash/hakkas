<template>
  <!-- Binding a process port to an environment channel (06 §4.14): an inport to a sensor, an
       outport to an actuator. From a block port that no process port exposes, it first makes one
       (its name asked, the block port's offered), then binds it: one undo step, and the port
       removed again if the binding is refused. It stays open until the binding's event arrives;
       a refusal is shown in it. -->
  <Popup :onOk="send" :onCancel="cancel" okLabel="Bind">
    <h4>Bind to environment</h4>
    <p class="hk-dialog-note">{{ note }}</p>
    <div v-if="block !== undefined" class="hk-field">
      <label class="hk-label" for="hk-bind-name">Process {{ direction === "in" ? "input" : "output" }}</label>
      <input id="hk-bind-name" ref="nameField" class="hk-input" data-testid="bind-port-name" v-model="name"
             spellcheck="false" autocomplete="off" :aria-invalid="nameProblem !== ''" @keydown.enter.prevent="send"/>
      <p v-if="nameProblem" class="hk-inspector-note" role="alert" data-testid="bind-name-problem">{{ nameProblem }}</p>
    </div>
    <div class="hk-field">
      <label class="hk-label" for="hk-bind-channel">Channel</label>
      <select id="hk-bind-channel" ref="field" class="hk-input" data-testid="bind-channel" v-model="channel"
              @keydown.enter.prevent="send">
        <optgroup v-for="group in groups" :key="group.port" :label="group.label">
          <option v-for="c in group.channels" :key="c.label" :value="c.label" :disabled="c.taken !== ''">
            {{ c.label }}{{ c.taken ? " — " + c.taken : "" }}
          </option>
        </optgroup>
      </select>
      <p v-if="groups.length === 0" class="hk-inspector-note" data-testid="bind-none">
        The environment has no {{ direction === "in" ? "sensors" : "actuators" }}.</p>
      <p v-if="error" class="hk-inspector-note" role="alert" data-testid="bind-error">{{ error }}</p>
    </div>
  </Popup>
</template>

<script lang="ts" setup>
import {computed, nextTick, onMounted, onUnmounted, ref, shallowRef, watch} from "vue";
import Popup from "@/components/Popup.vue";
import {errorClaims, vueModel} from "@/components/model";
import {bindingLabel, bindingsAt, boundTo, environment} from "@/components/environment";
import {channelsOfPort, parseChannel} from "@/components/bindings";
import type {Binding} from "@/components/protocols/environment";
import {begin, cancelGroup, end, unrecorded} from "./history";
import {editor} from "./viewmodel/default";
import {onPortShown, processPortNodes} from "./ports";

// `port`: the process port to bind; or `block` and `blockPort`: the block port to expose first.
const props = defineProps<{ direction: "in" | "out", port?: string, block?: string, blockPort?: string }>();
const emit = defineEmits<{ close: [] }>();

const graph = () => vueModel.currentGraphId.value;
const processName = (id: string) => vueModel.graphs.value.get(id)?.name ?? id;
const env = () => vueModel.connection.value?.proto.environment;
const graphProto = () => vueModel.connection.value?.proto.graph;

const taken = (n: string) => processPortNodes(editor).some(p => p.direction === props.direction && p.name === n);
const name = ref(props.port ?? (props.blockPort !== undefined && taken(props.blockPort)
    ? `${props.block}_${props.blockPort}` : props.blockPort ?? ""));
const nameProblem = computed(() => props.block === undefined ? ""
    : name.value === "" ? "A name is needed" : taken(name.value) ? "Port exists: " + name.value : "");

const note = computed(() => props.block !== undefined
    ? `${props.block}.${props.blockPort} — exposed as a process ${props.direction === "in" ? "input, fed by a sensor" : "output, driving an actuator"}`
    : `Process ${props.direction === "in" ? "input" : "output"} ${props.port} — ${props.direction === "in"
        ? "a sensor feeds it" : "it drives an actuator"}`);

// The channels of the right kind by port: an actuator already driven cannot take a second
// driver (04 §4); a sensor may feed any number of inports.
const groups = computed(() => {
  const d = environment.description;
  if (d === undefined)
    return [];
  return (props.direction === "in" ? d.outPorts : d.inPorts).map(p => ({
    port: p.id,
    label: p.id + (p.unit ? " (" + p.unit + ")" : "") + (p.description ? " — " + p.description : ""),
    channels: channelsOfPort(d, p.id).map(c => {
      const driver = props.direction === "in" ? undefined : boundTo(c.env, c.index)[0];
      const taken = driver === undefined ? ""
          : "driven by " + (driver.graph === graph() ? "" : processName(driver.graph) + ": ") + driver.port;
      return {...c, taken};
    }),
  }));
});

const channel = ref("");
const error = ref("");
const field = ref<HTMLSelectElement | null>(null);
const nameField = ref<HTMLInputElement | null>(null);
// What is under way: the port being made (`adding`), then the binding (`binding`, reactive: the
// dialog closes when its event is in).
let adding = false;
const binding = shallowRef<Binding | undefined>();
let stopWaiting: (() => void) | undefined;

onMounted(async () => {
  channel.value = groups.value.flatMap(g => g.channels).find(c => c.taken === "")?.label ?? "";
  await nextTick();
  if (props.block !== undefined) {
    nameField.value?.focus();
    nameField.value?.select();
  } else {
    field.value?.focus();
  }
});

const bind = (port: string) => {
  const {env: id, index} = parseChannel(channel.value);
  binding.value = index !== undefined ? {graph: graph(), port, env: id, index} : {graph: graph(), port, env: id};
  env()?.request_bind(binding.value);
};

const send = () => {
  if (channel.value === "" || adding || binding.value !== undefined || nameProblem.value !== "")
    return;
  error.value = "";
  if (props.block === undefined) {
    bind(props.port!);
    return;
  }
  // The port first; the binding once its event is in. One step for undo (06 §4.13).
  const port = name.value;
  adding = true;
  begin();
  stopWaiting = onPortShown((direction, shown) => {
    if (direction !== props.direction || shown !== port || !adding)
      return;
    adding = false;
    stopWaiting?.();
    bind(port);
  });
  graphProto()?.request_addport(props.direction, props.block, props.blockPort!, port);
};

const finish = () => {
  stopWaiting?.();
  if (props.block !== undefined)
    end();
  emit("close");
};

const cancel = () => {
  if (adding || binding.value !== undefined)
    return;   // under way: its answer closes the dialog or shows the refusal
  emit("close");
};

// Done when the binding's event is in.
watch(() => binding.value === undefined ? [] : bindingsAt(graph(), props.direction, binding.value.port).map(bindingLabel), labels => {
  if (binding.value !== undefined && labels.includes(bindingLabel(binding.value)))
    finish();
});

const bindRefusal = /^(Invalid id|Unknown environment port|Index required|No index expected|Index out of range|Unknown port|Invalid binding direction|Input already connected|Actuator already driven|No environment|Not permitted: environment:bind)/;
const portRefusal = /^(Port exists|Invalid id|Unknown port|Unknown node|Not permitted: graph:add(in|out)port)/;
const claim = (message: string): boolean => {
  if (adding && portRefusal.test(message)) {
    // The port was not made: nothing to undo.
    adding = false;
    stopWaiting?.();
    cancelGroup();
    error.value = message;
    return true;
  }
  if (binding.value === undefined || !bindRefusal.test(message))
    return false;
  if (props.block !== undefined) {
    // The port made for it goes again, and the step with it (06 §4.14).
    const port = binding.value.port;
    cancelGroup();
    unrecorded(() => graphProto()?.request_removeport(props.direction, port));
  }
  binding.value = undefined;
  error.value = message;
  return true;
};
errorClaims.push(claim);
onUnmounted(() => {
  stopWaiting?.();
  errorClaims.splice(errorClaims.indexOf(claim), 1);
});
</script>
