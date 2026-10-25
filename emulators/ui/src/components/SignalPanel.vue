<template>
  <div class="panel" data-testid="signal-panel">
    <section v-for="g in groups" :key="g.part.id" class="part" data-testid="part" :data-part="g.part.id">
      <h2>
        <span class="place">{{ g.part.id }}</span>
        <span class="name">{{ g.part.name ?? g.part.type }}</span>
        <span v-if="g.part.builtin" class="tag">built in</span>
      </h2>
      <table v-if="g.rows.length > 0">
        <thead>
          <tr>
            <th class="c-label">Channel</th>
            <th class="c-value">Value</th>
            <th class="c-control">Source</th>
            <th class="c-noise">Noise</th>
          </tr>
        </thead>
        <tbody>
          <RowView v-for="row in g.rows" :key="row.key" :row="row" :client="client"/>
        </tbody>
      </table>
      <p v-else class="nothing">Nothing to emulate.</p>
    </section>
    <p v-if="empty.length > 0" class="empties" data-testid="empty-slots">
      Empty: {{ empty.join(', ') }}
    </p>
  </div>
</template>

<script setup lang="ts">
import {computed} from 'vue';
import type {Client} from '../protocol/client';
import RowView from './RowView.vue';

const props = defineProps<{client: Client}>();
const scene = computed(() => props.client.mirror.scene);

// The filled parts in the scene's order, each with its rows.
const groups = computed(() => scene.value.parts
  .filter(p => p.type !== null)
  .map(part => ({part, rows: scene.value.rows.filter(r => r.part === part.id)})));
const empty = computed(() => scene.value.parts.filter(p => p.type === null).map(p => p.id));
</script>

<style scoped>
.panel {
  display: flex;
  flex-direction: column;
  gap: 14px;
  max-width: 1100px;
}

.part {
  background: var(--panel);
  border: 1px solid var(--line);
  border-radius: var(--radius);
  overflow: hidden;
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

.place {
  font-family: var(--mono);
  color: var(--muted);
  font-weight: normal;
}

.tag {
  font-size: 11px;
  font-weight: normal;
  color: var(--muted);
  border: 1px solid var(--line);
  border-radius: 999px;
  padding: 0 8px;
}

table {
  width: 100%;
  border-collapse: collapse;
}

th {
  text-align: left;
  font-weight: normal;
  font-size: 12px;
  color: var(--muted);
  padding: 4px 14px;
}

.c-label {
  width: 22%;
}

.c-value {
  width: 14%;
}

.c-noise {
  width: 12%;
}

.nothing, .empties {
  color: var(--muted);
  margin: 0;
  padding: 8px 14px;
}

.empties {
  padding: 0 2px;
  font-family: var(--mono);
  font-size: 12px;
}
</style>
