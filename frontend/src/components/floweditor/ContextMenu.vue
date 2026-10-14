<template>
  <div ref="root" class="hk-menu" data-testid="context-menu" :style="{left: left + 'px', top: top + 'px'}"
       @pointerdown.stop @contextmenu.prevent.stop @keydown="key">
    <template v-if="!searching">
      <div role="menu" aria-label="Actions">
        <button v-for="(item, i) in items" :key="item.action" ref="buttons" type="button" role="menuitem"
                class="hk-menu-item" data-testid="menu-item" :data-action="item.action" tabindex="-1"
                @click="choose(item.action)" @pointerenter="focus(i)">
          <span>{{ item.label }}</span>
          <span v-if="item.shortcut" class="hk-menu-shortcut">{{ item.shortcut }}</span>
        </button>
      </div>
    </template>
    <template v-else>
      <input ref="search" class="hk-input" data-testid="block-search" v-model="text" placeholder="Search block types"
             role="combobox" aria-expanded="true" aria-controls="hk-block-results" aria-autocomplete="list"
             :aria-activedescendant="matches.length > 0 ? 'hk-block-' + active : undefined"
             spellcheck="false" autocomplete="off"/>
      <ul id="hk-block-results" class="hk-menu-results" role="listbox" aria-label="Block types">
        <li v-for="(name, i) in matches" :id="'hk-block-' + i" :key="name" role="option"
            class="hk-menu-item" :class="{active: i === active}" :aria-selected="i === active"
            data-testid="block-result" :data-type="name" @click="emit('addBlock', name)" @pointerenter="active = i">
          {{ name }}
        </li>
        <li v-if="matches.length === 0" class="hk-empty" data-testid="block-none">No block type matches</li>
      </ul>
    </template>
  </div>
</template>

<script lang="ts" setup>
import {computed, nextTick, onMounted, ref, watch} from "vue";
import {MenuAction, MenuItem, nextIndex, searchTypes} from "./menus";

// A context menu (06 §4.4) at x, y in the editor pane. "Add block…" turns it into a search
// over the block types; the parent carries out what is chosen and closes it.
const props = defineProps<{ items: MenuItem[], x: number, y: number, types: string[] }>();
const emit = defineEmits<{ action: [action: MenuAction], addBlock: [type: string], close: [] }>();

const buttons = ref<HTMLButtonElement[]>([]);
const root = ref<HTMLElement | null>(null);
// Moved left or up when it would leave the pane (also once it shows the block search).
const left = ref(props.x);
const top = ref(props.y);
const fit = () => {
  const pane = root.value?.offsetParent as HTMLElement | null;
  if (root.value == null || pane == null)
    return;
  left.value = Math.max(0, Math.min(props.x, pane.clientWidth - root.value.offsetWidth - 4));
  top.value = Math.max(0, Math.min(props.y, pane.clientHeight - root.value.offsetHeight - 4));
};
const search = ref<HTMLInputElement | null>(null);
const searching = ref(false);
const text = ref("");
const active = ref(0);
let current = 0;

const matches = computed(() => searchTypes(props.types, text.value));
watch(text, () => active.value = 0);

const focus = (i: number) => {
  current = i;
  buttons.value[i]?.focus();
};

const choose = async (action: MenuAction) => {
  if (action === "add-block") {
    searching.value = true;
    await nextTick();
    fit();
    search.value?.focus();
    return;
  }
  emit("action", action);
};

const key = (event: KeyboardEvent) => {
  if (event.key === "Escape") {
    event.preventDefault();
    event.stopPropagation();
    emit("close");
    return;
  }
  if (event.key === "Tab") {
    event.preventDefault();
    return;
  }
  // Keys go to the menu, not to the editor (Delete, Ctrl+A …).
  event.stopPropagation();
  const move = ["ArrowDown", "ArrowUp", "Home", "End"].includes(event.key);
  if (searching.value) {
    if (move && event.key !== "Home" && event.key !== "End") {
      event.preventDefault();
      active.value = nextIndex(active.value, matches.value.length, event.key as "ArrowDown" | "ArrowUp");
    } else if (event.key === "Enter") {
      event.preventDefault();
      const name = matches.value[active.value];
      if (name !== undefined)
        emit("addBlock", name);
    }
    return;
  }
  if (move) {
    event.preventDefault();
    focus(nextIndex(current, props.items.length, event.key as "ArrowDown" | "ArrowUp" | "Home" | "End"));
  }
};

onMounted(() => {
  fit();
  focus(0);
});
</script>
