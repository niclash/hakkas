<template>
  <!-- The device window (06 §4.15, plan task 78): the carrier's picture with its modules in their
       slots and every channel's live value on them; in the page, or popped out into a window of
       its own. -->
  <Teleport :to="popTarget ?? 'body'" :disabled="popTarget === undefined">
    <section class="hk-device" :class="{popped: popTarget !== undefined}" data-testid="device-window"
             role="dialog" aria-label="Device">
      <header class="hk-device-header">
        <h2 class="hk-panel-title" data-testid="device-title">{{ title }}</h2>
        <a v-if="carrier?.documentation" class="hk-device-help" :href="carrier.documentation" target="_blank"
           rel="noopener noreferrer" data-testid="device-help">Help</a>
        <button v-if="popTarget === undefined" type="button" class="hk-button" data-testid="device-popout"
                @click="popOut">Pop out</button>
        <button type="button" class="hk-button" data-testid="device-close" @click="close">Close</button>
      </header>
      <p v-if="message" class="hk-inspector-note" role="status" data-testid="device-message">{{ message }}</p>
      <svg v-if="board" class="hk-device-board" :viewBox="`0 0 ${W} ${board.h}`" data-testid="device-board">
        <BoardImage :loaded="board.loaded" :picture="carrier?.picture" :rect="{x: 0, y: 0, w: W, h: board.h}"
                    data-testid="device-carrier"/>
        <component :is="part.documentation ? 'a' : 'g'" v-for="part in view.parts" :key="part.key"
                   :href="part.documentation" target="_blank" rel="noopener noreferrer"
                   class="hk-device-part" :class="part.kind" data-testid="device-part" :data-key="part.key">
          <title>{{ part.title }}</title>
          <BoardImage v-if="images[part.key]" :loaded="images[part.key]!" :picture="part.picture" :rect="scaled(part.rect)"/>
          <template v-else>
            <rect class="hk-device-empty" :x="scaled(part.rect).x" :y="scaled(part.rect).y"
                  :width="scaled(part.rect).w" :height="scaled(part.rect).h"/>
            <text class="hk-device-label" :x="(part.rect.x + part.rect.w / 2) * W" :y="(part.rect.y + part.rect.h / 2) * board.h"
                  :transform="`rotate(-90 ${(part.rect.x + part.rect.w / 2) * W} ${(part.rect.y + part.rect.h / 2) * board.h})`">
              {{ part.label }}</text>
          </template>
        </component>
        <g v-for="b in view.badges" :key="key(b)" class="hk-device-badge" :class="badgeClass(b)"
           data-testid="device-badge" :data-channel="label(b)" :data-value="badgeText(value(b))">
          <title>{{ badgeTitle(b) }}</title>
          <circle v-if="b.port.type === 'bool'" :cx="b.x * W" :cy="y(b)" r="7"/>
          <template v-else>
            <rect :x="b.x * W - textWidth(b) / 2" :y="y(b) - 8" :width="textWidth(b)" height="16" rx="4"/>
            <text :x="b.x * W" :y="y(b) + 3.5">{{ badgeText(value(b)) }}</text>
          </template>
        </g>
      </svg>
      <p v-else-if="!loading" class="hk-inspector-note" data-testid="device-no-picture">No picture of the device.</p>
      <div v-if="view.others.length > 0" class="hk-device-others" data-testid="device-others">
        <div v-for="o in view.others" :key="o.label" class="hk-device-other">
          <h3>{{ o.label }}</h3>
          <ul>
            <li v-for="c in o.channels" :key="key(c)" :class="badgeClass(c)" data-testid="device-other-channel"
                :data-channel="label(c)" :title="badgeTitle(c)">
              <span class="hk-env-channel">{{ label(c) }}</span>
              <span class="hk-device-value">{{ valueText(value(c), c.port.unit) }}</span>
            </li>
          </ul>
        </div>
      </div>
    </section>
  </Teleport>
</template>

<script lang="ts" setup>
import {computed, onBeforeUnmount, onMounted, reactive, ref, shallowRef, watch} from "vue";
import BoardImage from "@/components/BoardImage.vue";
import {vueModel} from "@/components/model";
import {boundTo, environment} from "@/components/environment";
import {channelLabel} from "@/components/bindings";
import {Badge, badgeText, device, layout, valueKey, valueText, watchDevice} from "@/components/device";
import {Loaded, picture, shownSize} from "@/components/pictures";
import type {EnvPort, EnvRect} from "@/components/protocols/environment";

// The picture's width in SVG units; its height follows the carrier's picture.
const W = 1000;

const d = computed(() => environment.description);
const carrier = computed(() => d.value?.carrier);
const view = computed(() => d.value !== undefined ? layout(d.value) : {parts: [], badges: [], others: []});
const title = computed(() => "Device: " + (carrier.value?.name ?? carrier.value?.type ?? d.value?.name ?? ""));
const message = ref("");

// ---- Pictures --------------------------------------------------------------------------

const board = shallowRef<{ loaded: Loaded, h: number } | undefined>();
const images = reactive<Record<string, Loaded | undefined>>({});
const loading = ref(true);

watch(d, async desc => {
  loading.value = true;
  const c = desc?.carrier;
  const loaded = await picture(c?.picture);
  if (loaded !== undefined) {
    const s = shownSize(c?.picture, loaded);
    board.value = {loaded, h: Math.round(W * s.height / s.width)};
  } else {
    board.value = undefined;
  }
  loading.value = false;
  for (const part of view.value.parts)
    picture(part.picture).then(l => images[part.key] = l);
}, {immediate: true});

const scaled = (r: EnvRect): EnvRect => ({x: r.x * W, y: r.y * (board.value?.h ?? 0), w: r.w * W, h: r.h * (board.value?.h ?? 0)});

// ---- Channels ----------------------------------------------------------------------------

type Channel = { env: string, index?: number, port: EnvPort, sensor: boolean };
const key = (c: Channel) => valueKey(c.env, c.index);
const label = (c: Channel) => channelLabel(c.env, c.index);
const value = (c: Channel) => device.values[key(c)];
const textWidth = (c: Channel) => 6 + 6 * badgeText(value(c)).length;
// Badges at one point are stacked downwards.
const y = (b: Badge) => b.y * (board.value?.h ?? 0) + b.stack * 18;

// A sensor or actuator; on or off; an actuator never written is not driven.
const badgeClass = (c: Channel) => {
  const v = value(c);
  return [c.sensor ? "sensor" : "actuator", v === undefined ? "none" : v === true ? "on" : v === false ? "off" : ""];
};

const badgeTitle = (c: Channel) => {
  const v = value(c);
  const state = v === undefined ? (c.sensor ? "no value yet" : "not driven") : valueText(v, c.port.unit);
  const bound = boundTo(c.env, c.index).map(b => (vueModel.graphs.value.get(b.graph)?.name ?? b.graph) + ": " + b.port);
  return [`${label(c)} (${c.sensor ? "in" : "out"}): ${state}`, c.port.description,
    bound.length > 0 ? "bound to " + bound.join(", ") : "free"].filter(s => s).join("\n");
};

// ---- The window --------------------------------------------------------------------------

const popTarget = shallowRef<HTMLElement | undefined>();
let popup: Window | null = null;

// Into a window of its own: the page's styles and theme copied, the content teleported, so the
// one connection serves both (06 §4.15). Closing that window closes the device window.
const popOut = () => {
  popup = window.open("", "hakkas-device", "popup,width=1100,height=760");
  if (popup === null) {
    message.value = "The browser did not open a window (pop-ups blocked?).";
    return;
  }
  const doc = popup.document;
  doc.title = title.value;
  doc.head.replaceChildren(...[...document.head.querySelectorAll("style, link[rel=stylesheet]")]
      .map(n => doc.importNode(n, true)));
  doc.documentElement.dataset.theme = document.documentElement.dataset.theme ?? "";
  doc.body.replaceChildren();
  doc.body.className = "hk-device-window";
  const root = doc.createElement("div");
  doc.body.append(root);
  popup.addEventListener("pagehide", close);
  popTarget.value = root;
  device.popped = true;
  message.value = "";
};

const close = () => {
  device.open = false;
};

const closePopup = () => {
  if (popup !== null) {
    popup.removeEventListener("pagehide", close);
    popup.close();
    popup = null;
  }
  popTarget.value = undefined;
  device.popped = false;
};

onMounted(() => {
  watchDevice(vueModel.connection.value, true);
  window.addEventListener("pagehide", closePopup);
});
onBeforeUnmount(() => {
  watchDevice(vueModel.connection.value, false);
  window.removeEventListener("pagehide", closePopup);
  closePopup();
});
</script>
