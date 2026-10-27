import {defineConfig, devices} from '@playwright/test';

// The User's Manual's screenshots (plan Section U, docs/manual/): scripts in tests/manual that
// drive the built frontend against a real engine, as the end-to-end tests do (their fixtures),
// and write PNGs into docs/manual/images/screenshots/. `npm run manual:screenshots` builds the
// frontend first; HAKKAS_BROWSER as for the tests.
export default defineConfig({
  testDir: 'tests/manual',
  testMatch: /\.shot\.ts$/,
  globalSetup: './tests/e2e/global-setup.ts',
  fullyParallel: true,
  timeout: 60000,
  expect: {timeout: 10000},
  reporter: [['list']],
  outputDir: 'test-results/manual',
  use: {
    ...devices['Desktop Chrome'],
    viewport: {width: 1400, height: 860},
    deviceScaleFactor: 1,
    launchOptions: process.env.HAKKAS_BROWSER ? {executablePath: process.env.HAKKAS_BROWSER} : {},
  },
});
