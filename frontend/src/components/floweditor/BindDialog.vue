<template>
  <!-- Binding a block port to an environment channel (06 §4.14): a sensor's for an input, an
       actuator's for an output. It stays open until the binding's event arrives; a refusal is
       shown in it. -->
  <Popup :onOk="send" :onCancel="() => emit('close')" okLabel="Bind">
    <h4>Bind to environment</h4>
    <p class="hk-dialog-note">{{ block }}.{{ port }} — {{ sensor ? "an input: a sensor feeds it" : "an output: it drives an actuator" }}</p>
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
        The environment has no {{ sensor ? "sensors" : "actuators" }}.</p>
      <p v-if="error" class="hk-inspector-note" role="alert" data-testid="bind-error">{{ error }}</p>
    </div>
  </Popup>
</template>

<script lang="ts" setup>
import {computed, nextTick, onMounted, onUnmounted, ref, watch} from "vue";
import Popup from "@/components/Popup.vue";
import {errorClaims, vueModel} from "@/components/model";
import {bindingAt, boundTo, environment} from "@/components/environment";
import {channelsOfPort, parseChannel} from "@/components/bindings";

const props = defineProps<{ block: string, port: string, sensor: boolean }>();
const emit = defineEmits<{ close: [] }>();

const graph = () => vueModel.currentGraphId.value;
const processName = (id: string) => vueModel.graphs.value.get(id)?.name ?? id;

// The channels of the right kind by port: an actuator already driven cannot take a second
// driver (04 §4); a sensor may feed any number of inputs.
const groups = computed(() => {
  const d = environment.description;
  if (d === undefined)
    return [];
  return (props.sensor ? d.outPorts : d.inPorts).map(p => ({
    port: p.id,
    label: p.id + (p.unit ? " (" + p.unit + ")" : "") + (p.description ? " — " + p.description : ""),
    channels: channelsOfPort(d, p.id).map(c => {
      const driver = props.sensor ? undefined : boundTo(c.env, c.index)[0];
      const taken = driver === undefined ? ""
          : "driven by " + (driver.graph === graph() ? "" : processName(driver.graph) + ": ") + driver.block + "." + driver.port;
      return {...c, taken};
    }),
  }));
});

const channel = ref("");
const error = ref("");
const field = ref<HTMLSelectElement | null>(null);
let pending = false;

onMounted(async () => {
  channel.value = groups.value.flatMap(g => g.channels).find(c => c.taken === "")?.label ?? "";
  await nextTick();
  field.value?.focus();
});

const send = () => {
  if (channel.value === "")
    return;
  const {env, index} = parseChannel(channel.value);
  error.value = "";
  pending = true;
  vueModel.connection.value?.proto.environment.request_bind(
      index !== undefined ? {graph: graph(), block: props.block, port: props.port, env, index}
          : {graph: graph(), block: props.block, port: props.port, env});
};

// Done when the binding's event is in.
watch(() => bindingAt(graph(), props.block, props.port), b => {
  if (b !== undefined && pending)
    emit("close");
});

const refusal = /^(Invalid id|Unknown environment port|Index required|No index expected|Index out of range|Unknown node|Unknown port|Invalid binding direction|Input already connected|Actuator already driven|No environment|Not permitted: environment:bind)/;
const claim = (message: string): boolean => {
  if (!pending || !refusal.test(message))
    return false;
  pending = false;
  error.value = message;
  return true;
};
errorClaims.push(claim);
onUnmounted(() => errorClaims.splice(errorClaims.indexOf(claim), 1));
</script>
