<template>
  <div class="stats" data-testid="stats-panel">
    <div class="head">
      <p class="intro">
        Each module serves hakkas' writes at its turn in the scan cycle, from a queue of limited size; a write that
        finds the queue full is <strong>dropped</strong>. Dropped writes mean the program writes faster than the
        device serves the module.
      </p>
      <button type="button" data-testid="reset-stats" @click="reset">Reset counters</button>
    </div>
    <p v-if="counters.parts.length === 0" class="empty">No modules.</p>
    <section v-for="p in counters.parts" :key="p.id" class="part" data-testid="stats-part" :data-part="p.id"
             :class="{dropping: p.dropped > 0}">
      <h2>
        <span class="place">{{ p.id }}</span> {{ p.name }}
        <span class="queue" data-testid="stats-queue">queue {{ p.queue }}, highest {{ p.queueMax }}</span>
        <span v-if="p.dropped > 0" class="dropped-badge" data-testid="stats-part-dropped">{{ p.dropped }} dropped</span>
      </h2>
      <table v-if="channelsOf(p.id).length > 0">
        <thead>
          <tr><th>Channel</th><th>Writes received</th><th>Queued</th><th>Dropped</th><th>Values sent</th></tr>
        </thead>
        <tbody>
          <tr v-for="c in channelsOf(p.id)" :key="c.key" data-testid="stats-channel" :data-key="c.key"
              :class="{dropping: (c.dropped ?? 0) > 0}">
            <td class="mono">{{ c.label }}</td>
            <td class="num">{{ c.output ? c.received : '' }}</td>
            <td class="num">{{ c.output ? c.queued : '' }}</td>
            <td class="num" data-testid="stats-dropped">{{ c.output ? c.dropped : '' }}</td>
            <td class="num">{{ c.output ? '' : c.sent }}</td>
          </tr>
        </tbody>
      </table>
    </section>
  </div>
</template>

<script setup lang="ts">
import {computed} from 'vue';
import type {Client} from '../protocol/client';
import {countersOf} from '../settings';

const props = defineProps<{client: Client}>();
const counters = computed(() => countersOf(props.client.mirror.stats, props.client.mirror.scene));
const channelsOf = (part: string) => counters.value.channels.filter(c => c.part === part);

function reset(): void {
  props.client.request('resetstats').catch((e: Error) => props.client.notify({level: 'error', message: e.message}));
}
</script>

<style scoped>
.stats {
  display: flex;
  flex-direction: column;
  gap: 14px;
  max-width: 900px;
}

.head {
  display: flex;
  align-items: flex-start;
  gap: 20px;
}

.intro {
  margin: 0;
  flex: 1;
  color: var(--muted);
}

.part {
  background: var(--panel);
  border: 1px solid var(--line);
  border-radius: var(--radius);
  overflow: hidden;
}

.part.dropping {
  border-color: var(--error);
}

h2 {
  display: flex;
  align-items: baseline;
  gap: 10px;
  margin: 0;
  padding: 8px 14px;
  font-size: 14px;
  border-bottom: 1px solid var(--line);
}

.place, .mono, .num {
  font-family: var(--mono);
}

.place {
  color: var(--muted);
  font-weight: normal;
}

.queue {
  margin-left: auto;
  font-weight: normal;
  font-size: 12px;
  color: var(--muted);
}

.dropped-badge {
  font-size: 12px;
  color: #fff;
  background: var(--error);
  border-radius: 999px;
  padding: 0 8px;
}

table {
  width: 100%;
  border-collapse: collapse;
  table-layout: fixed;    /* the columns line up from module to module */
}

th:first-child {
  width: 30%;
}

th {
  text-align: left;
  font-weight: normal;
  font-size: 12px;
  color: var(--muted);
  padding: 4px 14px;
}

td {
  padding: 4px 14px;
  border-top: 1px solid var(--line);
}

tr.dropping td {
  color: var(--error);
  font-weight: 600;
}

.empty {
  color: var(--muted);
}
</style>
