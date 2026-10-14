<template>
  <div v-if="secret !== ''" class="hk-app">
    <header class="hk-topbar">
      <img alt="Bali Automation logotype" src="@/assets/logo.svg" class="hk-logo"/>
      <span class="hk-product">Hakkas</span>
      <span class="hk-spacer"></span>
      <span v-if="!reconnect && roleLabel(can)" class="hk-role" data-testid="role">{{ roleLabel(can) }}</span>
      <span class="hk-user" data-testid="user">{{ vueModel.user.value }} · {{ vueModel.role.value }}</span>
      <button type="button" class="hk-button" data-testid="messages-toggle" :aria-pressed="showMessages"
              @click="toggleMessages">Messages<span v-if="unread > 0" class="hk-badge" data-testid="messages-unread">{{ unread }}</span></button>
      <button type="button" class="hk-button" data-testid="about-open" @click="showAbout = true">About</button>
      <ThemeSelector/>
      <button class="hk-button" data-testid="logout" @click="logout()">Logout</button>
    </header>
    <div class="hk-errors" v-if="vueModel.errors.value.length > 0">
      <div class="hk-error" role="alert" data-testid="error" v-for="(error, i) in vueModel.errors.value" :key="i">
        <span>{{ error }}</span>
        <button type="button" class="hk-icon-button" aria-label="Dismiss" @click="vueModel.errors.value.splice(i, 1)">&times;</button>
      </div>
    </div>
    <div v-if="reconnect" class="hk-reconnecting" role="status" data-testid="reconnecting">
      <span>The connection to the engine was lost. Reconnecting{{ reconnect.attempt > 1 ? " (attempt " + reconnect.attempt + ")" : "" }}…
        Changes are possible again once it is back.</span>
      <button type="button" class="hk-button" data-testid="reconnect-now" @click="attempt()">Retry now</button>
    </div>
    <!-- Rebuilt for every connection: its listeners are the connection's. -->
    <FlowEditor :key="connectionCount" :secret="secret"></FlowEditor>
    <MessagesPanel v-if="showMessages" @close="toggleMessages"/>
    <AboutDialog v-if="showAbout" @close="showAbout = false"/>
  </div>
  <Login v-else @login="login" :busy="loggingIn" :resuming="resuming" :message="loginMessage"/>
</template>

<script lang="ts" setup>
import FlowEditor from "./components/floweditor/FlowEditor.vue";
import {computed, ref, watch} from "vue";
import Login from "@/components/Login.vue";
import ThemeSelector from "@/components/ThemeSelector.vue";
import MessagesPanel from "@/components/MessagesPanel.vue";
import AboutDialog from "@/components/AboutDialog.vue";
import {Connection} from "@/components/protocols/websocket";
import {attachModel, can, errorClaims, messageLog, vueModel} from "@/components/model";
import {addressFor, Opened, parseAddress, titleFor} from "@/address";
import {roleLabel} from "@/components/roles";
import {OnLogin, OnLoginFailed} from "@/components/protocols/environment";
import {NetworkErrorEvent} from "@/components/protocols/network";
import {forgetSession, keepSession, keptSession, reconnectDelay} from "@/session";

// The session secret; "" while logged out.
const secret = ref("");
const loggingIn = ref(false);
const loginMessage = ref("");
// Resuming the kept session (06 §2): a failure only shows the login form.
const resuming = ref(false);
// Reconnecting after a lost connection (06 §2): the attempt, while it lasts. The editor stays,
// read-only, until the session is resumed on a new connection.
const reconnect = ref<{ attempt: number, process: string } | undefined>();
let reconnectTimer: ReturnType<typeof setTimeout> | undefined;
// Counts the connections that logged in: the editor is rebuilt for each.
const connectionCount = ref(0);

// Every login uses a fresh connection, which ends at logout.
const login = (user: string, password: string) => {
  resuming.value = false;
  connect(conn => conn.proto.environment.request_login(user, password));
}

// A reload resumes the kept session instead of asking for the password (02 §2.3.1).
const resume = () => {
  const session = keptSession();
  if (session === undefined)
    return;
  resuming.value = true;
  connect(conn => conn.proto.environment.request_resume(session));
}

const connect = (start: (conn: Connection) => void) => {
  vueModel.connection.value?.close();
  loggingIn.value = true;
  loginMessage.value = "";

  let conn = new Connection();
  vueModel.connection.value = conn;
  // While reconnecting, the model keeps what it shows until the session is back (onLogin).
  if (reconnect.value === undefined)
    attachModel(conn);
  conn.proto.environment.addListener("onLogin", onLogin);
  conn.proto.environment.addListener("onLoginFailed", (c: Connection, failure: OnLoginFailed) => loginFailed(c, failure.message));
  conn.proto.environment.addListener("onLogout", onLogout);
  conn.proto.network.addListener("onNetworkError", (e: NetworkErrorEvent) => {
    if (secret.value === "" || reconnect.value !== undefined)
      loginFailed(conn, e.message);
  });
  conn.addOpenedListener(() => start(conn));
  conn.addClosedListener(onClosed);
  conn.open();
}

// A reconnecting attempt: resume the kept session on a new connection.
const attempt = () => {
  clearTimeout(reconnectTimer);
  const r = reconnect.value;
  const session = keptSession();
  if (r === undefined)
    return;
  if (session === undefined) {
    endSession("The connection was lost and the session has ended: log in again.");
    return;
  }
  r.attempt++;
  connect(conn => conn.proto.environment.request_resume(session));
}

const scheduleAttempt = () => {
  const r = reconnect.value;
  if (r === undefined)
    return;
  clearTimeout(reconnectTimer);
  reconnectTimer = setTimeout(attempt, reconnectDelay(r.attempt + 1));
}

const endSession = (message: string) => {
  clearTimeout(reconnectTimer);
  reconnect.value = undefined;
  secret.value = "";
  loginMessage.value = message;
}

const loginFailed = (conn: Connection, message: string) => {
  if (conn !== vueModel.connection.value)
    return;
  conn.proto.environment.abandon_login();
  conn.close();
  vueModel.connection.value = undefined;
  loggingIn.value = false;
  if (reconnect.value !== undefined) {
    // The session has ended meanwhile (it expired, the engine restarted): log in again.
    forgetSession();
    endSession("The connection was lost and the session has ended: log in again.");
    return;
  }
  if (resuming.value) {
    // The session has ended (or its id was used by another tab): log in again.
    forgetSession();
    resuming.value = false;
    message = "";
  }
  loginMessage.value = message;
}

const logout = () => {
  forgetSession();
  if (reconnect.value !== undefined) {
    // Nothing to tell an engine that is not there.
    vueModel.connection.value?.close();
    vueModel.connection.value = undefined;
    endSession("");
    return;
  }
  vueModel.connection.value?.proto.environment.request_logout();
}

// After a logout or a session expiry.
const onLogout = (conn: Connection) => {
  forgetSession();
  secret.value = "";
  conn.close();
  if (vueModel.connection.value === conn)
    vueModel.connection.value = undefined;
}

const onLogin = (conn: Connection, payload: OnLogin) => {
  // The id just used is gone: keep the new one.
  if (payload.resume !== undefined)
    keepSession(payload.resume);
  else
    forgetSession();
  const reopen = reconnect.value?.process ?? "";
  if (reconnect.value !== undefined) {
    // Back: the model starts again from this connection's messages.
    clearTimeout(reconnectTimer);
    reconnect.value = undefined;
    attachModel(conn);
  }
  conn.currentSecret = payload.secret;
  vueModel.capabilities.value = payload.capabilities ?? [];
  vueModel.user.value = payload.user;
  vueModel.role.value = payload.role;
  secret.value = payload.secret;
  connectionCount.value++;
  loggingIn.value = false;
  resuming.value = false;
  vueModel.errors.value = [];
  conn.proto.runtime.request_getruntime();
  // The process that was open, again (its watches follow, 06 §4.7); else the one the address
  // names (06 §2.1: a bookmark, a reload).
  if (reopen !== "")
    conn.proto.graph.request_connect(reopen);
  else
    openAddress(conn);
};

// ---- Messages and About (06 §4.12) --------------------------------------------------------

const showMessages = ref(false);
const showAbout = ref(false);
// Messages that came while the panel was closed.
const seen = ref(0);
const unread = computed(() => showMessages.value ? 0 : messageLog.items.filter(m => m.seq > seen.value).length);
const toggleMessages = () => {
  showMessages.value = !showMessages.value;
  seen.value = messageLog.items[messageLog.items.length - 1]?.seq ?? seen.value;
};
watch(() => messageLog.items[messageLog.items.length - 1]?.seq, last => {
  if (showMessages.value && last !== undefined)
    seen.value = last;
});

// ---- The address (06 §2.1) ----------------------------------------------------------------

// What is open now, as the address names it.
const opened = computed<Opened | undefined>(() => {
  const id = vueModel.currentGraphId.value;
  if (id === "")
    return undefined;
  return {kind: vueModel.currentComponent.value?.id === id ? "type" : "process", id};
});

// Opening something adds it to the browser's history (unless the history moved there). Nothing
// open (after a login, before the address's process opens) leaves the address as it is.
let fromHistory = false;
watch(opened, now => {
  if (now === undefined)
    return;
  const address = addressFor(now);
  if (location.pathname + location.search !== address) {
    if (fromHistory)
      history.replaceState(null, "", address);
    else
      history.pushState(null, "", address);
  }
  fromHistory = false;
});

watch(() => {
  const id = vueModel.currentGraphId.value;
  return vueModel.currentComponent.value?.id === id && id !== ""
    ? vueModel.currentComponent.value?.name : vueModel.graphs.value.get(id)?.name;
}, name => document.title = titleFor(name), {immediate: true});

// The address's process or type, asked for; one the engine does not have takes the address
// back to "/" (its refusal is shown as usual).
let asked = "";
const openAddress = (conn: Connection) => {
  const wanted = parseAddress(location.search);
  if (wanted === undefined || wanted.id === vueModel.currentGraphId.value) {
    fromHistory = false;
    return;
  }
  asked = wanted.id;
  conn.proto.graph.request_connect(wanted.id);
};
errorClaims.push((message: string) => {
  if (asked !== "" && message === "Unknown graph: " + asked) {
    asked = "";
    history.replaceState(null, "", "/");
  }
  return false;
});

window.addEventListener("popstate", () => {
  const conn = vueModel.connection.value;
  if (conn === undefined || secret.value === "")
    return;
  fromHistory = true;
  openAddress(conn);
});

const onClosed = (conn: Connection, event: CloseEvent) => {
  if (conn !== vueModel.connection.value)
    return;
  vueModel.connection.value = undefined;
  loggingIn.value = false;
  resuming.value = false;
  if (reconnect.value !== undefined) {
    scheduleAttempt();   // the engine is not reachable yet
    return;
  }
  if (secret.value !== "" && keptSession() !== undefined) {
    // Lost while logged in: keep the editor, read-only, and reconnect (06 §2).
    vueModel.capabilities.value = [];
    reconnect.value = {attempt: 0, process: vueModel.currentGraphId.value};
    scheduleAttempt();
    return;
  }
  if (secret.value === "" && loginMessage.value === "")
    loginMessage.value = "Cannot reach the server" + (event.reason ? ": " + event.reason : "");
  else if (secret.value !== "")
    loginMessage.value = "The connection to the server was lost";
  secret.value = "";
};

resume();
</script>
