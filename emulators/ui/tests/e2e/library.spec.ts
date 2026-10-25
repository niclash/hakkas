import type {Page} from '@playwright/test';
import {readFileSync} from 'node:fs';
import {join} from 'node:path';
import {expect, LAB, test} from './fixtures';

// The environment library, the settings and the counters (Colibri spec §8.2; plan task E13):
// every change goes to the emulator and comes back as its state.

const PUMP = {carrier: 'colibri_8', modules: [{slot: 1, type: 'colibri_dp1'}]};
test.use({environments: {lab: LAB, pump: PUMP}});

const item = (page: Page, name: string) => page.locator(`[data-testid=env-item][data-name="${name}"]`);
const setting = (page: Page, id: string) => page.locator(`[data-testid=setting][data-id="${id}"]`);
const input = (page: Page, id: string) => setting(page, id).getByTestId('setting-input');
const file = (dir: string, name: string) => JSON.parse(readFileSync(join(dir, 'environments', name + '.json'), 'utf8'));

async function nameDialog(page: Page, name: string) {
    await page.getByTestId('env-name-field').fill(name);
    await page.getByTestId('dialog-ok').click();
}

test('environments are created, duplicated, renamed and deleted', async ({page, emulator}) => {
    await page.goto(emulator.url);
    await expect(page.getByTestId('env-item')).toHaveCount(2);
    await expect(item(page, 'lab').getByTestId('env-running')).toBeVisible();
    await expect(item(page, 'lab').getByTestId('env-delete')).toHaveCount(0);   // the running one cannot be

    await page.getByTestId('env-new').click();
    await nameDialog(page, 'bench');
    await expect(page.getByTestId('dialog')).toHaveCount(0);
    await expect(item(page, 'bench')).toBeVisible();
    expect(file(emulator.library, 'bench')).toMatchObject({carrier: 'colibri_8', modules: []});

    // A name taken, or not a name: the refusal in the dialog.
    await page.getByTestId('env-new').click();
    await nameDialog(page, 'lab');
    await expect(page.getByTestId('dialog-error')).toHaveText('Environment exists: lab');
    await nameDialog(page, '.hidden');
    await expect(page.getByTestId('dialog-error')).toHaveText('Invalid name');
    await page.getByTestId('dialog-cancel').click();

    await item(page, 'lab').getByTestId('env-duplicate').click();
    await expect(page.getByTestId('env-name-field')).toHaveValue('lab-copy');
    await page.getByTestId('dialog-ok').click();
    await expect(item(page, 'lab-copy')).toBeVisible();
    expect(file(emulator.library, 'lab-copy').modules).toHaveLength(5);

    await item(page, 'pump').getByTestId('env-rename').click();
    await nameDialog(page, 'pump-room');
    await expect(item(page, 'pump-room')).toBeVisible();
    await expect(item(page, 'pump')).toHaveCount(0);

    await item(page, 'lab-copy').getByTestId('env-delete').click();
    await page.getByTestId('dialog-ok').click();
    await expect(item(page, 'lab-copy')).toHaveCount(0);
    await expect(page.getByTestId('env-item')).toHaveCount(3);
});

test('settings change at once; Persist writes them; another environment asks about unsaved changes', async ({page, emulator}) => {
    await page.goto(emulator.url);
    const control = await emulator.control();
    await page.getByTestId('tab-settings').click();
    await expect(page.getByTestId('persist')).toBeDisabled();
    await input(page, 'cycleMs').fill('20');
    await input(page, 'cycleMs').press('Enter');
    await expect(page.getByTestId('unsaved')).toBeVisible();
    await expect(page.getByTestId('persist')).toBeEnabled();
    expect((await control.request('persist', {})).ok).toBeDefined();   // another client persists
    await expect(page.getByTestId('unsaved')).toHaveCount(0);
    expect(file(emulator.library, 'lab').cycleMs).toBe(20);

    // A refusal, shown as a notice; the field shows the emulator's value again.
    await input(page, 'queueSize').fill('0');
    await input(page, 'queueSize').press('Enter');
    await expect(page.getByTestId('notice')).toContainText('Invalid value: queueSize');
    await input(page, 'queueSize').blur();
    await expect(input(page, 'queueSize')).toHaveValue('10');

    // Unsaved changes, then another environment: discarded.
    await input(page, 'cycleMs').fill('30');
    await input(page, 'cycleMs').press('Enter');
    await expect(page.getByTestId('unsaved')).toBeVisible();
    await item(page, 'pump').getByTestId('env-select').click();
    await expect(page.getByTestId('dialog')).toContainText('lab has unsaved changes');
    await page.getByTestId('dialog-discard').click();
    await expect(page.getByTestId('environment')).toHaveText('pump');
    expect(file(emulator.library, 'lab').cycleMs).toBe(20);

    // And back, persisting first.
    await input(page, 'cycleMs').fill('40');
    await input(page, 'cycleMs').press('Enter');
    await expect(page.getByTestId('unsaved')).toBeVisible();
    await item(page, 'lab').getByTestId('env-select').click();
    await page.getByTestId('dialog-persist').click();
    await expect(page.getByTestId('environment')).toHaveText('lab');
    expect(file(emulator.library, 'pump').cycleMs).toBe(40);
    await expect(input(page, 'cycleMs')).toHaveValue('20');
});

test('modules per slot, the built-in slot fixed; a power cycle asks to restart hakkas', async ({page, emulator}) => {
    await page.goto(emulator.url);
    const hakkas = await emulator.hakkas();
    await expect(page.getByTestId('link')).toHaveText('hakkas: connected');
    await page.getByTestId('tab-settings').click();
    await expect(input(page, 'slot0.type')).toBeDisabled();
    await expect(setting(page, 'slot0.type').getByTestId('restart-mark')).toHaveCount(0);
    await expect(setting(page, 'slot6.type').getByTestId('restart-mark')).toBeVisible();
    await expect(setting(page, 'slot6.serial')).toHaveCount(0);
    await input(page, 'slot6.type').selectOption({label: 'Colibri SSR'});
    // The serial field comes with the module.
    await expect(setting(page, 'slot6.serial')).toBeVisible();
    await input(page, 'slot6.serial').fill('SSR-6');
    await input(page, 'slot6.serial').press('Enter');
    await expect(page.getByTestId('restart-notice')).toContainText('restart hakkas');
    await expect(page.getByTestId('link')).toHaveText('hakkas: restart needed');
    await page.getByTestId('tab-signals').click();
    await expect(page.locator('[data-testid=part][data-part="slot6"] h2')).toContainText('Colibri SSR');
    hakkas.close();
});

test('a communication module brings its parameters', async ({page, emulator}) => {
    await page.goto(emulator.url);
    await page.getByTestId('tab-settings').click();
    await expect(setting(page, 'comm1.wifi.ssid')).toHaveCount(0);
    await input(page, 'comm1.type').selectOption({label: 'Colibri WiFi'});
    await input(page, 'comm1.wifi.ssid').fill('plant');
    await input(page, 'comm1.wifi.ssid').press('Enter');
    await input(page, 'comm1.ip.dns').fill('1.1.1.1, 9.9.9.9');
    await input(page, 'comm1.ip.dns').press('Enter');
    await input(page, 'comm1.ip.mode').selectOption({label: 'Static'});
    await page.getByTestId('persist').click();
    await expect(page.getByTestId('unsaved')).toHaveCount(0);
    expect(file(emulator.library, 'lab').comm).toEqual([{slot: 1, type: 'colibri_wifi',
        ip: {dns: ['1.1.1.1', '9.9.9.9'], mode: 'static'}, wifi: {ssid: 'plant'}}]);
});

test('the counters show dropped writes, and are reset', async ({page, emulator}) => {
    await page.goto(emulator.url);
    await page.getByTestId('tab-settings').click();
    await input(page, 'queueSize').fill('2');
    await input(page, 'queueSize').press('Enter');
    await expect(page.getByTestId('unsaved')).toBeVisible();
    const hakkas = await emulator.hakkas();
    for (let i = 0; i < 40; i++)
        hakkas.write('slot3-dq', 1, i % 2 === 0);
    await page.getByTestId('tab-stats').click();
    const part = page.locator('[data-testid=stats-part][data-part="slot3"]');
    await expect(part.getByTestId('stats-part-dropped')).toBeVisible();
    await expect(page.getByTestId('tab-dropped')).toBeVisible();
    const row = page.locator('[data-testid=stats-channel][data-key="slot3.dq.1"]');
    await expect(row).toHaveClass(/dropping/);
    await expect.poll(async () => Number(await row.getByTestId('stats-dropped').textContent())).toBeGreaterThan(0);
    await page.getByTestId('reset-stats').click();
    await expect(row.getByTestId('stats-dropped')).toHaveText('0');
    await expect(part.getByTestId('stats-part-dropped')).toHaveCount(0);
    await expect(page.getByTestId('tab-dropped')).toHaveCount(0);
    hakkas.close();
});
