<template>
  <label class="hk-theme" v-if="themes.length > 1">
    Theme
    <select class="hk-select" data-testid="theme" v-model="current" @change="applyTheme(current)">
      <option v-for="theme in themes" :key="theme.id" :value="theme.id">{{ theme.name }}</option>
    </select>
  </label>
</template>

<script setup lang="ts">
import {onMounted, ref} from "vue";
import {applyTheme, installedThemes, ThemeInfo} from "@/theme";

// The installed themes (06 §4.1); the page already shows the current one (index.html,
// settled at startup by main.ts).
const themes = ref<ThemeInfo[]>([]);
const current = ref("");

onMounted(async () => {
  try {
    themes.value = await installedThemes();
  } catch (e) {
    console.log("No theme list:", e);
    return;
  }
  current.value = document.documentElement.dataset.theme ?? "";
});
</script>
