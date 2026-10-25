<template>
  <div class="app">
    <header class="top">
      <h1 data-testid="title">{{ title }}</h1>
      <span v-if="m.library?.running" class="env" data-testid="environment">
        {{ m.library.running }}<span v-if="m.library.unsaved" class="unsaved" title="Unsaved changes"
                                     data-testid="unsaved">●</span>
      </span>
      <button v-if="m.library?.running" type="button" class="primary" data-testid="persist"
              :disabled="!m.library.unsaved" title="Write the environment's file: its structure, settings, sources and noise"
              @click="persist">Persist</button>
      <span class="spacer"></span>
      <span class="badge" :class="linkClass" data-testid="link" :title="linkTitle">{{ linkText }}</span>
    </header>

    <div v-if="m.connection !== 'open'" class="banner" data-testid="connection">
      {{ connectionText }}
    </div>

    <div v-if="m.link === 'restart'" class="banner restart" data-testid="restart-notice">
      The emulated device changed its structure (a power cycle): <strong>restart hakkas</strong> so that it sees the new
      environment. Until then hakkas keeps its environment IO stopped.
    </div>

    <div class="body">
      <LibraryPanel :client="client"/>
      <main>
        <p v-if="m.connection === 'open' && m.library && !m.library.running" class="empty" data-testid="no-environment">
          No environment is running: run one from the list.
        </p>
        <template v-else>
          <nav class="tabs" role="tablist">
            <button v-for="t in tabs" :key="t.id" type="button" role="tab" :aria-selected="tab === t.id"
                    :class="{active: tab === t.id}" :data-testid="'tab-' + t.id" @click="choose(t.id)">
              {{ t.label }}<span v-if="t.id === 'stats' && dropped > 0" class="dropped" data-testid="tab-dropped">{{ dropped }}</span>
            </button>
          </nav>
          <SignalPanel v-if="tab === 'signals'" :client="client"/>
          <SettingsPanel v-else-if="tab === 'settings'" :client="client"/>
          <StatsPanel v-else :client="client"/>
        </template>
      </main>
    </div>

    <ul class="notices" aria-live="polite">
      <li v-for="n in m.notices" :key="n.id" :class="n.level" data-testid="notice">
        <span>{{ n.message }}</span>
        <button class="close" title="Dismiss" aria-label="Dismiss" @click="client.dismiss(n.id)">×</button>
      </li>
    </ul>
  </div>
</template>

<script setup lang="ts">
import {computed, ref, watchEffect} from 'vue';
import LibraryPanel from './components/LibraryPanel.vue';
import SettingsPanel from './components/SettingsPanel.vue';
import SignalPanel from './components/SignalPanel.vue';
import StatsPanel from './components/StatsPanel.vue';
import type {Client} from './protocol/client';

const props = defineProps<{client: Client}>();
const m = props.client.mirror;

// The tabs; the one chosen is the viewer's, kept in this browser where it can be.
type Tab = 'signals' | 'settings' | 'stats';
const tabs: {id: Tab, label: string}[] = [{id: 'signals', label: 'Signals'}, {id: 'settings', label: 'Settings'},
  {id: 'stats', label: 'Statistics'}];
const TAB_KEY = 'emulator.tab';
const stored = (() => {
  try {
    return localStorage.getItem(TAB_KEY);
  } catch {
    return null;
  }
})();
const tab = ref<Tab>(tabs.some(t => t.id === stored) ? stored as Tab : 'signals');
function choose(t: Tab): void {
  tab.value = t;
  try {
    localStorage.setItem(TAB_KEY, t);
  } catch {
    // Storage unavailable: the choice lasts for this page.
  }
}

// Writes dropped in all, shown on the statistics tab.
const dropped = computed(() => Object.values(m.stats.parts).reduce((n, p) => n + p.dropped, 0));

function persist(): void {
  props.client.request('persist').catch((e: Error) => props.client.notify({level: 'error', message: e.message}));
}

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

.banner.restart {
  background: var(--warn);
  color: #fff;
}

.body {
  display: grid;
  grid-template-columns: 260px 1fr;
  gap: 16px;
  padding: 16px 20px 40px;
  align-items: start;
}

main {
  min-width: 0;
}

.tabs {
  display: flex;
  gap: 4px;
  margin-bottom: 14px;
  border-bottom: 1px solid var(--line);
}

.tabs button {
  border: none;
  border-bottom: 2px solid transparent;
  border-radius: 0;
  background: none;
  padding: 6px 14px;
  color: var(--muted);
}

.tabs button.active {
  color: var(--text);
  border-bottom-color: var(--accent);
}

.dropped {
  margin-left: 6px;
  font-size: 11px;
  color: #fff;
  background: var(--error);
  border-radius: 999px;
  padding: 0 6px;
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
