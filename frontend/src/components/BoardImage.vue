<template>
  <!-- A board's picture drawn into rect as shown: its crop, turned about the rect's centre
       (06 §4.15, components/pictures.ts). -->
  <g :transform="d.transform">
    <svg :x="d.box.x" :y="d.box.y" :width="d.box.w" :height="d.box.h" :viewBox="d.viewBox"
         preserveAspectRatio="none">
      <image :href="loaded.src" x="0" y="0" width="1" height="1" preserveAspectRatio="none"/>
    </svg>
  </g>
</template>

<script lang="ts" setup>
import {computed} from "vue";
import type {EnvPicture, EnvRect} from "@/components/protocols/environment";
import {drawing, Loaded} from "@/components/pictures";

const props = defineProps<{ loaded: Loaded, picture?: EnvPicture, rect: EnvRect }>();
const d = computed(() => drawing(props.picture, props.loaded, props.rect));
</script>
