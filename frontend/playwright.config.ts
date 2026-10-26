import {defineConfig, devices} from '@playwright/test';

// End-to-end tests (tests/e2e): the built frontend in a real browser against a real hakkas
// engine, one engine per test (tests/e2e/fixtures.ts). `npm run test:e2e` builds the
// frontend first. The browser is Playwright's Chromium (`npx playwright install chromium`
// once), or any Chromium-based browser named by HAKKAS_BROWSER; HAKKAS_ALL_BROWSERS=1 also runs
// the clipboard suite in Firefox and WebKit.
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
    viewport: {width: 1500, height: 1000},
  },
  projects: [
    {
      name: 'chromium',
      use: {
        ...devices['Desktop Chrome'],
        viewport: {width: 1500, height: 1000},
        launchOptions: process.env.HAKKAS_BROWSER ? {executablePath: process.env.HAKKAS_BROWSER} : {},
      },
    },
    // The clipboard across browsers (06 §4.10, plan task 57), only when asked for:
    // HAKKAS_ALL_BROWSERS=1, after `npx playwright install firefox webkit`.
    ...(process.env.HAKKAS_ALL_BROWSERS ? ['firefox', 'webkit'].map(name => ({
      name,
      testMatch: /copypaste\.spec\.ts/,
      use: {...devices[name === 'firefox' ? 'Desktop Firefox' : 'Desktop Safari'], viewport: {width: 1500, height: 1000}},
    })) : []),
  ],
});
