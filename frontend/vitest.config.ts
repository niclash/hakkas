import {fileURLToPath, URL} from 'node:url'
import {defineConfig} from 'vitest/config'

// Unit tests (tests/unit): the protocol and model code in Node, without a browser or an
// engine. The end-to-end tests (tests/e2e) are Playwright's, playwright.config.ts.
export default defineConfig({
  resolve: {
    alias: {
      '@': fileURLToPath(new URL('./src', import.meta.url))
    }
  },
  test: {
    include: ['tests/unit/**/*.test.ts'],
    environment: 'node',
    // The protocol code logs every message; `npx vitest --silent=false` shows it.
    silent: true,
  },
});
