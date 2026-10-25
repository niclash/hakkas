<template>
  <aside class="library" data-testid="library">
    <div class="head">
      <h2>Environments</h2>
      <button type="button" data-testid="env-new" title="A new environment with the emulator's defaults"
              @click="open({kind: 'create'})">New…</button>
    </div>
    <ul>
      <li v-for="e in environments" :key="e.name" class="env" :class="{running: e.name === running}"
          data-testid="env-item" :data-name="e.name">
        <span class="name">{{ e.name }}</span>
        <span v-if="e.name === running" class="tag" data-testid="env-running">running</span>
        <span v-if="e.name === running && unsaved" class="unsaved" title="Unsaved changes">●</span>
        <span class="actions">
          <button v-if="e.name !== running" type="button" data-testid="env-select" title="Run this environment instead"
                  @click="select(e.name)">Run</button>
          <button type="button" data-testid="env-duplicate" title="A copy of its file (its saved state)"
                  @click="open({kind: 'duplicate', from: e.name})">Duplicate</button>
          <button type="button" data-testid="env-rename" @click="open({kind: 'rename', from: e.name})">Rename</button>
          <button v-if="e.name !== running" type="button" class="danger" data-testid="env-delete"
                  @click="open({kind: 'delete', from: e.name})">Delete</button>
        </span>
      </li>
    </ul>

    <ModalDialog v-if="dialog && dialog.kind !== 'delete' && dialog.kind !== 'unsaved'" :title="titles[dialog.kind]"
                 :ok-label="okLabels[dialog.kind]" :error="error" :busy="busy" @ok="submit" @cancel="close">
      <label class="field">Name
        <input ref="nameField" v-model="name" type="text" spellcheck="false" autocomplete="off" data-testid="env-name-field">
      </label>
      <p v-if="dialog.kind === 'rename' && dialog.from === running" class="note">
        Renaming the running environment power-cycles the emulated device: hakkas has to be restarted.</p>
    </ModalDialog>
    <ModalDialog v-if="dialog?.kind === 'delete'" :title="`Delete ${dialog.from}?`" ok-label="Delete" :danger="true"
                 :error="error" :busy="busy" @ok="submit" @cancel="close">
      <p>Its file is removed from the library.</p>
    </ModalDialog>
    <ModalDialog v-if="dialog?.kind === 'unsaved'" :title="`${running} has unsaved changes`" :error="error"
                 @ok="switchTo(true)" @cancel="close">
      <p>Running {{ dialog.from }} instead discards them, unless they are persisted first.</p>
      <template #actions>
        <button type="button" data-testid="dialog-cancel" @click="close">Cancel</button>
        <button type="button" data-testid="dialog-discard" :disabled="busy" @click="switchTo(false)">Discard</button>
        <button type="submit" class="primary" data-testid="dialog-persist" :disabled="busy">Persist, then run</button>
      </template>
    </ModalDialog>
  </aside>
</template>

<script setup lang="ts">
import {computed, nextTick, ref} from 'vue';
import type {Client} from '../protocol/client';
import ModalDialog from './ModalDialog.vue';

const props = defineProps<{client: Client}>();
const library = computed(() => props.client.mirror.library);
const environments = computed(() => library.value?.environments ?? []);
const running = computed(() => library.value?.running ?? null);
const unsaved = computed(() => library.value?.unsaved === true);

type Dialog = {kind: 'create'} | {kind: 'duplicate' | 'rename' | 'delete' | 'unsaved', from: string};
const dialog = ref<Dialog | undefined>();
const name = ref('');
const error = ref('');
const busy = ref(false);
const nameField = ref<HTMLInputElement | null>(null);

const titles = {create: 'New environment', duplicate: 'Duplicate environment', rename: 'Rename environment'};
const okLabels = {create: 'Create', duplicate: 'Duplicate', rename: 'Rename'};

async function open(d: Dialog): Promise<void> {
  dialog.value = d;
  error.value = '';
  busy.value = false;
  name.value = d.kind === 'create' ? '' : d.kind === 'duplicate' ? `${d.from}-copy` : d.from;
  await nextTick();
  nameField.value?.focus();
  nameField.value?.select();
}

function close(): void {
  dialog.value = undefined;
}

// Sends a request; done closes the dialog, a refusal is shown in it.
async function run(name: string, payload: Record<string, string | boolean>): Promise<boolean> {
  busy.value = true;
  error.value = '';
  try {
    await props.client.request(name, payload);
    return true;
  } catch (e) {
    error.value = (e as Error).message;
    return false;
  } finally {
    busy.value = false;
  }
}

async function submit(): Promise<void> {
  const d = dialog.value;
  if (d === undefined || busy.value)
    return;
  const ok = d.kind === 'create' ? await run('create', {name: name.value})
      : d.kind === 'duplicate' ? await run('create', {name: name.value, from: d.from})
      : d.kind === 'rename' ? await run('rename', {name: d.from, to: name.value})
      : d.kind === 'delete' ? await run('delete', {name: d.from})
      : false;
  if (ok)
    close();
}

// Runs another environment; with unsaved changes, the user decides first.
async function select(target: string): Promise<void> {
  if (unsaved.value) {
    await open({kind: 'unsaved', from: target});
    return;
  }
  try {
    await props.client.request('select', {name: target});
  } catch (e) {
    props.client.notify({level: 'error', message: (e as Error).message});
  }
}

async function switchTo(persist: boolean): Promise<void> {
  const d = dialog.value;
  if (d?.kind !== 'unsaved' || busy.value)
    return;
  if (persist && !await run('persist', {}))
    return;
  if (await run('select', {name: d.from, discard: !persist}))
    close();
}
</script>

<style scoped>
.library {
  background: var(--panel);
  border: 1px solid var(--line);
  border-radius: var(--radius);
  padding: 8px 0;
  align-self: start;
  position: sticky;
  top: 60px;
}

.head {
  display: flex;
  align-items: center;
  padding: 0 12px 6px;
  border-bottom: 1px solid var(--line);
}

h2 {
  flex: 1;
  margin: 0;
  font-size: 14px;
}

ul {
  list-style: none;
  margin: 0;
  padding: 4px 0;
}

.env {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 6px;
  padding: 5px 12px;
}

.env.running {
  background: var(--accent-soft);
}

.name {
  font-family: var(--mono);
}

.tag {
  font-size: 11px;
  color: var(--accent);
}

.unsaved {
  color: var(--warn);
}

.actions {
  display: flex;
  width: 100%;
  gap: 4px;
}

.actions button {
  font-size: 12px;
  padding: 1px 7px;
}

button.danger {
  color: var(--error);
}

.field {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.field input {
  width: 100%;
}

.note {
  color: var(--warn);
  margin: 10px 0 0;
}
</style>
