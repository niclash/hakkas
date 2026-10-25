import {createApp} from 'vue';
import App from './App.vue';
import {Client, type Socket} from './protocol/client';
import './style.css';

// The emulator that served this page; its control WebSocket is on the same origin (§1).
const scheme = location.protocol === 'https:' ? 'wss' : 'ws';
const client = new Client(`${scheme}://${location.host}/control`, url => new WebSocket(url) as unknown as Socket);
client.start();
createApp(App, {client}).mount('#app');
