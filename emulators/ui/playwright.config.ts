import {defineConfig, devices} from '@playwright/test';

// End-to-end tests (tests/e2e): the built UI in a real browser against a real Colibri emulator,
// one per test (tests/e2e/fixtures.ts). `npm run test:e2e` builds the UI first; the emulator
// must be built (`npm run build` in ../colibri). The browser is Playwright's Chromium
// (`npx playwright install chromium` once), or any Chromium-based browser named by
// EMULATOR_BROWSER (or HAKKAS_BROWSER).
const browser = process.env.EMULATOR_BROWSER ?? process.env.HAKKAS_BROWSER;

export default defineConfig({
  testDir: 'tests/e2e',
  globalSetup: './tests/e2e/global-setup.ts',
  fullyParallel: true,
  forbidOnly: !!process.env.CI,
  timeout: 30000,
  expect: {timeout: 5000},
  reporter: [['list'], ['html', {open: 'never', outputFolder: 'test-results/report'}]],
  outputDir: 'test-results/artifacts',
  use: {
    trace: 'retain-on-failure',
    screenshot: 'only-on-failure',
    viewport: {width: 1300, height: 900},
  },
  projects: [
    {
      name: 'chromium',
      use: {
        ...devices['Desktop Chrome'],
        viewport: {width: 1300, height: 900},
        launchOptions: browser ? {executablePath: browser} : {},
      },
    },
  ],
});
