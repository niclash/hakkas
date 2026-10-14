import { fileURLToPath, URL } from 'node:url'

import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

// https://vitejs.dev/config/
export default defineConfig({
  plugins: [
    vue(),
  ],
  build: {
    // The auto-arrange plugin's chunk (elkjs, about 1.4 MB, 0.4 MB compressed) is loaded only
    // when "Arrange blocks" is used (06 §5); the page's own script is about 0.3 MB.
    chunkSizeWarningLimit: 1500,
  },
  resolve: {
    alias: {
      '@': fileURLToPath(new URL('./src', import.meta.url))
    }
  }
});
