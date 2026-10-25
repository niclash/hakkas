<template>
  <div class="app">
    <header class="top">
      <h1 data-testid="title">{{ title }}</h1>
      <span v-if="m.library?.running" class="env" data-testid="environment">
        {{ m.library.running }}<span v-if="m.library.unsaved" class="unsaved" title="Unsaved changes"
                                     data-testid="unsaved">●</span>
      </span>
      <span class="spacer"></span>
      <span class="badge" :class="linkClass" data-testid="link" :title="linkTitle">{{ linkText }}</span>
    </header>

    <div v-if="m.connection !== 'open'" class="banner" data-testid="connection">
      {{ connectionText }}
    </div>

    <main>
      <p v-if="m.connection === 'open' && m.library && !m.library.running" class="empty" data-testid="no-environment">
        No environment is running.
      </p>
      <SignalPanel v-else :client="client"/>
    </main>

    <ul class="notices" aria-live="polite">
      <li v-for="n in m.notices" :key="n.id" :class="n.level" data-testid="notice">
        <span>{{ n.message }}</span>
        <button class="close" title="Dismiss" aria-label="Dismiss" @click="client.dismiss(n.id)">×</button>
      </li>
    </ul>
  </div>
</template>

<script setup lang="ts">
import {computed, watchEffect} from 'vue';
import SignalPanel from './components/SignalPanel.vue';
import type {Client} from './protocol/client';

const props = defineProps<{client: Client}>();
const m = props.client.mirror;

const title = computed(() => {
  const name = m.hello?.emulator ?? '';
  return name === '' ? 'Emulator' : `${name.charAt(0).toUpperCase()}${name.slice(1)} emulator`;
});
watchEffect(() => {
  document.title = m.library?.running ? `${m.library.running} — ${title.value}` : title.value;
});

const linkText = computed(() => ({waiting: 'hakkas: not connected', connected: 'hakkas: connected',
  restart: 'hakkas: restart needed'})[m.link ?? 'waiting']);
const linkClass = computed(() => m.link ?? 'waiting');
const linkTitle = computed(() => m.link === 'restart'
  ? 'The emulated device changed its structure: restart hakkas so it sees the new environment.' : '');

const connectionText = computed(() => ({
  connecting: 'Connecting to the emulator…',
  closed: 'The connection to the emulator was lost; reconnecting…',
  incompatible: `This UI does not speak the emulator's protocol (version ${m.hello?.protocol}).`,
  open: '',
})[m.connection]);
</script>

<style scoped>
.app {
  min-height: 100vh;
}

.top {
  display: flex;
  align-items: baseline;
  gap: 12px;
  padding: 10px 20px;
  background: var(--panel);
  border-bottom: 1px solid var(--line);
  position: sticky;
  top: 0;
  z-index: 2;
}

h1 {
  font-size: 17px;
  margin: 0;
}

.env {
  color: var(--muted);
  font-family: var(--mono);
}

.unsaved {
  color: var(--warn);
  margin-left: 4px;
}

.spacer {
  flex: 1;
}

.badge {
  font-size: 12px;
  padding: 2px 10px;
  border-radius: 999px;
  border: 1px solid var(--line);
  color: var(--muted);
}

.badge.connected {
  color: var(--ok);
  border-color: var(--ok);
}

.badge.restart {
  color: #fff;
  background: var(--warn);
  border-color: var(--warn);
}

.banner {
  padding: 8px 20px;
  background: var(--accent-soft);
  border-bottom: 1px solid var(--line);
}

main {
  padding: 16px 20px 40px;
}

.empty {
  color: var(--muted);
}

.notices {
  position: fixed;
  right: 16px;
  bottom: 16px;
  margin: 0;
  padding: 0;
  list-style: none;
  display: flex;
  flex-direction: column;
  gap: 8px;
  max-width: 420px;
  z-index: 3;
}

.notices li {
  display: flex;
  gap: 10px;
  align-items: flex-start;
  background: var(--panel);
  border: 1px solid var(--line);
  border-left: 4px solid var(--accent);
  border-radius: var(--radius);
  padding: 8px 10px;
  box-shadow: 0 4px 14px rgb(0 0 0 / 12%);
}

.notices li.warning {
  border-left-color: var(--warn);
}

.notices li.error {
  border-left-color: var(--error);
}

.notices span {
  flex: 1;
}

.close {
  border: none;
  padding: 0 4px;
  background: none;
  font-size: 16px;
  line-height: 1;
}
</style>
