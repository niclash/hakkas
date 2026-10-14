<template>
  <div class="hk-login">
    <div class="hk-login-card">
      <header>
        <img alt="" src="@/assets/logo.svg" class="hk-logo"/>
        <h1>Hakkas</h1>
      </header>
      <p class="hk-subtitle">Log in to see and edit the processes.</p>
      <form @submit.prevent="doLogin">
        <div class="hk-field">
          <label class="hk-label" for="user">User</label>
          <input class="hk-input" type="text" id="user" v-model="user" autocomplete="username" :disabled="busy">
        </div>
        <div class="hk-field">
          <label class="hk-label" for="password">Password</label>
          <input class="hk-input" type="password" id="password" v-model="password" autocomplete="current-password" :disabled="busy">
        </div>
        <button class="hk-button primary" type="submit" :disabled="busy || user === ''">{{ busy ? "Logging in..." : "Log in" }}</button>
        <p class="hk-login-message" role="alert" data-testid="login-message" v-if="message !== ''">{{ message }}</p>
      </form>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref } from "vue";

defineProps<{
  busy: boolean,
  message: string,
}>();

const emit = defineEmits<{
  login: [name: string, pass: string]
}>();

const user = ref("");
const password = ref("");

const doLogin = () => {
  emit('login', user.value, password.value);
  password.value = "";
}
</script>
