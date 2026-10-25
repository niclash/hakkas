import {fileURLToPath, URL} from 'node:url'
import {defineConfig} from 'vite'
import vue from '@vitejs/plugin-vue'

// `npm run dev` serves the UI with hot reloading and passes the control WebSocket and the
// assets on to a running emulator: EMULATOR_HTTP, default http://localhost:8090.
const emulator = process.env.EMULATOR_HTTP ?? 'http://localhost:8090';

export default defineConfig({
  plugins: [vue()],
  build: {
    // Not assets/: the emulator serves its plugins' pictures there.
    assetsDir: 'ui',
  },
  resolve: {
    alias: {'@': fileURLToPath(new URL('./src', import.meta.url))},
  },
  server: {
    proxy: {
      '/control': {target: emulator.replace(/^http/, 'ws'), ws: true},
      '/assets': {target: emulator},
    },
  },
});
