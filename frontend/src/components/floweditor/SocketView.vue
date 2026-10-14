<template>
  <!-- A port's socket, colored by its type (06 §4.5); the type shows on hover. -->
  <div class="socket hk-socket" :class="'hk-socket-' + kind" :title="data.name || 'any type'"
       data-testid="socket" :data-type="data.name"></div>
</template>

<script lang="ts" setup>
import {computed} from "vue";
import {ClassicPreset} from "rete";
import {socketKind} from "@/components/palette";

const props = defineProps<{ data: ClassicPreset.Socket }>();
const kind = computed(() => socketKind(props.data.name));
</script>
