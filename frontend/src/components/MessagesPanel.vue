<template>
  <section class="hk-messages" data-testid="messages" aria-label="Messages">
    <div class="hk-messages-bar">
      <h2 class="hk-panel-title">Messages</h2>
      <label class="hk-label hk-messages-level">Show
        <select class="hk-select" v-model="threshold" data-testid="messages-level">
          <option value="fine">everything</option>
          <option value="info">info and above</option>
          <option value="warn">warnings and errors</option>
          <option value="error">errors</option>
        </select>
      </label>
      <span class="hk-spacer"></span>
      <button type="button" class="hk-button small" data-testid="messages-clear" @click="messageLog.clear()">Clear</button>
      <button type="button" class="hk-icon-button" aria-label="Close the messages" data-testid="messages-close"
              @click="emit('close')">&times;</button>
    </div>
    <ol ref="list" class="hk-messages-list" aria-live="polite">
      <li v-for="m in items" :key="m.seq" class="hk-message" :class="m.level" data-testid="message" :data-level="m.level">
        <span class="hk-message-level">{{ m.refused ? "refused" : m.level }}</span>
        <span class="hk-message-text">{{ m.text }}</span>
        <span v-if="m.source" class="hk-message-source">{{ m.source }}</span>
      </li>
      <li v-if="items.length === 0" class="hk-empty" data-testid="messages-none">
        No messages. The engine sends its log here when <code>log.mirror</code> is on.
      </li>
    </ol>
  </section>
</template>

<script lang="ts" setup>
import {computed, nextTick, ref, watch} from "vue";
import {messageLog} from "@/components/model";
import {Level, shown} from "@/components/messages";

// The engine's mirrored log and the refusals (06 §4.12), newest at the bottom.
const emit = defineEmits<{ close: [] }>();
const threshold = ref<Level>("info");
const items = computed(() => shown(messageLog.items, threshold.value));
const list = ref<HTMLElement | null>(null);

// Follows the newest message, unless scrolled up.
watch(() => items.value.length, async () => {
  const el = list.value;
  const atBottom = el === null || el.scrollTop + el.clientHeight >= el.scrollHeight - 4;
  await nextTick();
  if (atBottom && el !== null)
    el.scrollTop = el.scrollHeight;
}, {immediate: true});
</script>
