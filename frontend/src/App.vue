<template>
  <div v-if="secret !== ''" class="hk-app">
    <header class="hk-topbar">
      <img alt="Bali Automation logotype" src="@/assets/logo.svg" class="hk-logo"/>
      <span class="hk-product">Hakkas</span>
      <span class="hk-spacer"></span>
      <ThemeSelector/>
      <button class="hk-button" data-testid="logout" @click="logout()">Logout</button>
    </header>
    <div class="hk-errors" v-if="vueModel.errors.value.length > 0">
      <div class="hk-error" role="alert" data-testid="error" v-for="(error, i) in vueModel.errors.value" :key="i">
        <span>{{ error }}</span>
        <button type="button" class="hk-icon-button" aria-label="Dismiss" @click="vueModel.errors.value.splice(i, 1)">&times;</button>
      </div>
    </div>
    <FlowEditor :secret="secret"></FlowEditor>
  </div>
  <Login v-else @login="login" :busy="loggingIn" :message="loginMessage"/>
</template>

<script lang="ts" setup>
import FlowEditor from "./components/floweditor/FlowEditor.vue";
import {ref} from "vue";
import Login from "@/components/Login.vue";
import ThemeSelector from "@/components/ThemeSelector.vue";
import {Connection} from "@/components/protocols/websocket";
import {attachModel, vueModel} from "@/components/model";
import {OnLogin, OnLoginFailed} from "@/components/protocols/environment";
import {NetworkErrorEvent} from "@/components/protocols/network";

// The session secret; "" while logged out.
const secret = ref("");
const loggingIn = ref(false);
const loginMessage = ref("");

// Every login uses a fresh connection, which ends at logout.
const login = (user: string, password: string) => {
  vueModel.connection.value?.close();
  loggingIn.value = true;
  loginMessage.value = "";

  let conn = new Connection();
  vueModel.connection.value = conn;
  attachModel(conn);
  conn.proto.environment.addListener("onLogin", onLogin);
  conn.proto.environment.addListener("onLoginFailed", (c: Connection, failure: OnLoginFailed) => loginFailed(c, failure.message));
  conn.proto.environment.addListener("onLogout", onLogout);
  conn.proto.network.addListener("onNetworkError", (e: NetworkErrorEvent) => {
    if (secret.value === "")
      loginFailed(conn, e.message);
  });
  conn.addOpenedListener(() => conn.proto.environment.request_login(user, password));
  conn.addClosedListener(onClosed);
  conn.open();
}

const loginFailed = (conn: Connection, message: string) => {
  if (conn !== vueModel.connection.value)
    return;
  conn.proto.environment.abandon_login();
  conn.close();
  vueModel.connection.value = undefined;
  loggingIn.value = false;
  loginMessage.value = message;
}

const logout = () => {
  vueModel.connection.value?.proto.environment.request_logout();
}

// After a logout or a session expiry.
const onLogout = (conn: Connection) => {
  secret.value = "";
  conn.close();
  if (vueModel.connection.value === conn)
    vueModel.connection.value = undefined;
}

const onLogin = (conn: Connection, payload: OnLogin) => {
  conn.currentSecret = payload.secret;
  secret.value = payload.secret;
  loggingIn.value = false;
  vueModel.errors.value = [];
  conn.proto.runtime.request_getruntime();
};

const onClosed = (conn: Connection, event: CloseEvent) => {
  if (conn !== vueModel.connection.value)
    return;
  vueModel.connection.value = undefined;
  loggingIn.value = false;
  if (secret.value === "" && loginMessage.value === "")
    loginMessage.value = "Cannot reach the server" + (event.reason ? ": " + event.reason : "");
  else if (secret.value !== "")
    loginMessage.value = "The connection to the server was lost";
  secret.value = "";
};

</script>
