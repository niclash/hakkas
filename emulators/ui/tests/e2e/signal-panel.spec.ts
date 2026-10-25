import type {Page} from '@playwright/test';
import {expect, test} from './fixtures';

// The signal panel (Colibri spec §8.2; plan task E12): one row per channel and signal, built
// from the scene alone; every change goes to the emulator and comes back as its state.

const row = (page: Page, key: string) => page.locator(`[data-testid=row][data-key="${key}"]`);
const value = (page: Page, key: string) => row(page, key).getByTestId('value');

test('shows the parts that are filled and a row per channel, from the scene', async ({page, emulator}) => {
    await page.goto(emulator.url);
    await expect(page.getByTestId('title')).toHaveText('Colibri emulator');
    await expect(page.getByTestId('environment')).toHaveText('lab');
    await expect(page).toHaveTitle('lab — Colibri emulator');
    await expect(page.getByTestId('link')).toHaveText('hakkas: not connected');
    const parts = page.getByTestId('part');
    await expect(parts).toHaveCount(6);     // the built-in slot 0 and slots 1-5
    await expect(parts.nth(1).locator('h2')).toContainText('slot1Colibri AIV');
    await expect(page.getByTestId('empty-slots')).toHaveText('Empty: slot6, slot7');
    await expect(row(page, 'slot1.ai.1')).toHaveAttribute('data-kind', 'input');
    await expect(row(page, 'slot1.ai.1')).toContainText('slot1-ai[1]');
    await expect(row(page, 'slot2.in.1')).toHaveAttribute('data-kind', 'signal');
    await expect(row(page, 'slot2.freq.1')).toHaveAttribute('data-kind', 'measured');
    await expect(row(page, 'slot3.dq.1')).toHaveAttribute('data-kind', 'output');
    // Units, and values with their resolution's decimals.
    await expect(value(page, 'slot1.ai.1')).toHaveText(/^0\.00\dV$/);
    await expect(value(page, 'slot4.ai')).toHaveText(/^0\.\dbar$|^0\.\dkPa$|^0\.\d/);
    await expect(row(page, 'slot1.ai.1').getByTestId('slider')).toBeVisible();
    await expect(row(page, 'slot2.freq.1').getByTestId('noise')).toBeVisible();
    await expect(row(page, 'slot2.count.1').getByTestId('noise')).toHaveCount(0);
});

test('a typed value and a slider set the input; the value comes back from the scan cycle', async ({page, emulator}) => {
    await page.goto(emulator.url);
    const control = await emulator.control();
    const number = row(page, 'slot1.ai.1').getByTestId('number');
    await number.fill('7.5');
    await number.press('Enter');
    await expect.poll(() => control.values['slot1.ai.1.source']).toBe(7.5);
    await expect(value(page, 'slot1.ai.1')).toHaveText(/^7\.(49\d|50\d)V$/);
    await expect(page.getByTestId('unsaved')).toBeVisible();
    await row(page, 'slot1.ai.2').getByTestId('slider').fill('2');
    await expect.poll(() => control.values['slot1.ai.2.source']).toBe(2);
    await expect(row(page, 'slot1.ai.2').getByTestId('number')).toHaveValue('2');
});

test('typing is not undone by the values that keep coming', async ({page, emulator}) => {
    await page.goto(emulator.url);
    const control = await emulator.control();
    await control.set('slot1.ai.1.noise', 0.5);     // its value changes at every turn
    const number = row(page, 'slot1.ai.1').getByTestId('number');
    await number.click();
    await number.press('Control+a');
    await number.pressSequentially('6.25', {delay: 120});
    await expect(number).toHaveValue('6.25');
    await number.press('Enter');
    await expect.poll(() => control.values['slot1.ai.1.source']).toBe(6.25);
    // Left, it follows the emulator again.
    await number.blur();
    await control.set('slot1.ai.1.source', 3);
    await expect(number).toHaveValue('3');
});

test('another client\'s changes appear', async ({page, emulator}) => {
    await page.goto(emulator.url);
    const control = await emulator.control();
    await control.set('slot1.ai.1.source', {sine: {offset: 5, amplitude: 2, period: 10}});
    await expect(row(page, 'slot1.ai.1').getByTestId('summary')).toHaveText('sine 5 ± 2 V, 10 s');
    await expect(row(page, 'slot1.ai.1').getByTestId('number')).toHaveCount(0);
    await control.set('slot1.ai.1.noise', 0.25);
    await expect(row(page, 'slot1.ai.1').getByTestId('noise')).toHaveValue('0.25');
    await control.set('slot5.di.2.source', true);
    await expect(row(page, 'slot5.di.2').getByTestId('toggle')).toBeChecked();
});

test('the source editor sets a function, and Cancel changes nothing', async ({page, emulator}) => {
    await page.goto(emulator.url);
    const control = await emulator.control();
    const r = row(page, 'slot1.ai.1');
    await r.getByTestId('edit-source').click();
    const editor = page.getByTestId('source-editor');
    await editor.getByTestId('kind').selectOption('sine');
    await editor.getByTestId('p-period').fill('4');
    await editor.getByTestId('p-period').press('Tab');
    await editor.getByTestId('apply').click();
    await expect(editor).toHaveCount(0);
    await expect.poll(() => control.values['slot1.ai.1.source']).toEqual({sine: {offset: 5, amplitude: 5, period: 4}});
    await expect(r.getByTestId('summary')).toHaveText('sine 5 ± 5 V, 4 s');

    // A draft that is cancelled is not sent.
    await r.getByTestId('edit-source').click();
    await editor.getByTestId('kind').selectOption('ramp');
    await editor.getByTestId('cancel').click();
    await expect(r.getByTestId('summary')).toHaveText('sine 5 ± 5 V, 4 s');

    // Back to a constant: the control comes back.
    await r.getByTestId('edit-source').click();
    await editor.getByTestId('kind').selectOption('constant');
    await editor.getByTestId('apply').click();
    await expect(r.getByTestId('number')).toBeVisible();
});

test('a sequence\'s steps are edited, added and removed', async ({page, emulator}) => {
    await page.goto(emulator.url);
    const control = await emulator.control();
    await row(page, 'slot1.ai.1').getByTestId('edit-source').click();
    const editor = page.getByTestId('source-editor');
    await editor.getByTestId('kind').selectOption('sequence');
    await editor.getByTestId('add-step').click();
    await expect(editor.getByTestId('step')).toHaveCount(3);
    await editor.getByTestId('step-value').nth(2).fill('3');
    await editor.getByTestId('step-value').nth(2).press('Tab');
    await editor.getByTestId('step').nth(0).getByRole('button', {name: '−'}).click();
    await editor.getByTestId('apply').click();
    await expect.poll(() => control.values['slot1.ai.1.source'])
        .toEqual({sequence: {steps: [[1, 10], [1, 3]], repeat: true}});
});

test('a pulse train on a digital input drives its measured channels', async ({page, emulator}) => {
    await page.goto(emulator.url);
    const control = await emulator.control();
    await row(page, 'slot2.in.1').getByTestId('edit-source').click();
    const editor = page.getByTestId('source-editor');
    await editor.getByTestId('p-frequency').getByTestId('p-value').fill('25');
    await editor.getByTestId('p-frequency').getByTestId('p-value').press('Tab');
    await editor.getByTestId('p-duty').getByTestId('p-value').fill('50');
    await editor.getByTestId('p-duty').getByTestId('p-value').press('Tab');
    await editor.getByTestId('apply').click();
    await expect.poll(() => control.values['slot2.in.1.source']).toEqual({pulse: {frequency: 25, duty: 50}});
    await expect(row(page, 'slot2.in.1').getByTestId('summary')).toHaveText('pulses 25 Hz, duty 50 %');
    await expect(value(page, 'slot2.freq.1')).toHaveText(/^2[45]\.\d\dHz$/);
    await expect(value(page, 'slot2.duty.1')).toHaveText(/^50\.0%$/);
    await expect.poll(async () => Number(await value(page, 'slot2.count.1').textContent())).toBeGreaterThan(5);
});

test('an output shows what hakkas writes, at its slot\'s turn', async ({page, emulator}) => {
    await page.goto(emulator.url);
    const hakkas = await emulator.hakkas();
    await expect(page.getByTestId('link')).toHaveText('hakkas: connected');
    const lamp = row(page, 'slot3.dq.2').getByTestId('lamp');
    await expect(lamp).toHaveAttribute('aria-label', 'off');
    hakkas.write('slot3-dq', 2, true);
    await expect(lamp).toHaveAttribute('aria-label', 'on');
    await expect(lamp).toHaveClass(/on/);
    hakkas.close();
    await expect(page.getByTestId('link')).toHaveText('hakkas: not connected');
});

test('a toggle sets a bool input', async ({page, emulator}) => {
    await page.goto(emulator.url);
    const control = await emulator.control();
    await row(page, 'slot5.di.1').getByTestId('toggle').check();
    await expect.poll(() => control.values['slot5.di.1.source']).toBe(true);
    await expect.poll(() => control.values['slot5.di.1']).toBe(true);
    await expect(value(page, 'slot5.di.1')).toHaveText('on');
});

test('noise is set per row', async ({page, emulator}) => {
    await page.goto(emulator.url);
    const control = await emulator.control();
    const noise = row(page, 'slot4.ai').getByTestId('noise');
    await noise.fill('2');
    await noise.press('Enter');
    await expect.poll(() => control.values['slot4.ai.noise']).toBe(2);
    // One that is not a number is put back, and nothing is sent.
    await noise.fill('');
    await noise.press('Enter');
    await expect(noise).toHaveValue('2');
});

test('a refusal is shown as a notice', async ({page, emulator}) => {
    await page.goto(emulator.url);
    await row(page, 'slot1.ai.1').getByTestId('edit-source').click();
    const editor = page.getByTestId('source-editor');
    await editor.getByTestId('kind').selectOption('sine');
    await editor.getByTestId('p-period').fill('0');
    await editor.getByTestId('p-period').press('Tab');
    await editor.getByTestId('apply').click();
    const notice = page.getByTestId('notice');
    await expect(notice).toContainText('Invalid source: slot1.ai.1.source: sine.period: expected a number > 0');
    await notice.getByRole('button', {name: 'Dismiss'}).click();
    await expect(notice).toHaveCount(0);
});

test('a lost connection is shown, and the controls stay as they were', async ({page, emulator, pageErrors}) => {
    await page.goto(emulator.url);
    await expect(row(page, 'slot1.ai.1')).toBeVisible();
    await emulator.stop();
    await expect(page.getByTestId('connection')).toHaveText('The connection to the emulator was lost; reconnecting…');
    await expect(row(page, 'slot1.ai.1')).toBeVisible();
    // The browser reports the failed reconnection attempts.
    await page.waitForTimeout(100);
    pageErrors.splice(0);
});
