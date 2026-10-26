import type {Page} from '@playwright/test';
import {expect, test} from './fixtures';

// The device as a picture (Colibri spec §8.4; plan task E14): the carrier's copy with each module
// in its place and each channel at its point, operable there; the list below stays complete.

const point = (page: Page, key: string) => page.locator(`[data-testid=board-point][data-key="${key}"]`);
const row = (page: Page, key: string) => page.locator(`[data-testid=row][data-key="${key}"]`);

test('shows the board with its parts and the channels at their points', async ({page, emulator}) => {
    await page.goto(emulator.url);
    const board = page.getByTestId('board');
    await expect(board).toBeVisible();
    await expect(board.locator('svg > image')).toHaveAttribute('href', 'assets/carriers/colibri_8/picture.webp');
    await expect(page.locator('[data-testid=board-part][data-part=slot1] image'))
        .toHaveAttribute('href', 'assets/modules/colibri_aiv/picture.webp');
    // The MCU module (default colibri_mcu6) as decoration; empty slots show the board's own.
    await expect(page.getByTestId('board-extra')).toHaveCount(1);
    await expect(page.locator('[data-testid=board-part][data-part=slot6]')).toHaveCount(0);
    // Every picture loads (the emulator serves the copies).
    const broken = await page.locator('svg image').evaluateAll(images => Promise.all(images.map(i =>
        fetch((i as SVGImageElement).href.baseVal).then(r => r.status))));
    expect(broken.every(s => s === 200)).toBe(true);
    await expect(point(page, 'slot1.ai.1')).toHaveAttribute('data-value', /^0(\.00\d+)?$/);
    await expect(point(page, 'slot3.dq.1')).toHaveClass(/output/);
});

test('a number input opens its slider where it is; a value set there comes back', async ({page, emulator}) => {
    await page.goto(emulator.url);
    const control = await emulator.control();
    await point(page, 'slot1.ai.1').click();
    const popover = page.getByTestId('board-popover');
    await expect(popover).toContainText('slot1-ai[1]');
    await popover.getByTestId('board-number').fill('4.5');
    await popover.getByTestId('board-number').press('Enter');
    await expect.poll(() => control.values['slot1.ai.1.source']).toBe(4.5);
    await expect(point(page, 'slot1.ai.1')).toHaveAttribute('data-value', /^4\.5$|^4\.49$|^4\.51$/);
    await popover.getByTestId('board-slider').fill('2');
    await expect.poll(() => control.values['slot1.ai.1.source']).toBe(2);
    // "In the list…" closes it and marks the row.
    await popover.getByTestId('board-row').click();
    await expect(popover).toHaveCount(0);
    await expect(row(page, 'slot1.ai.1')).toHaveClass(/flash/);
});

test('an output follows what hakkas writes', async ({page, emulator}) => {
    await page.goto(emulator.url);
    const hakkas = await emulator.hakkas();
    await expect(point(page, 'slot3.dq.1')).toHaveClass(/off/);
    hakkas.write('slot3-dq', 1, true);
    await expect(point(page, 'slot3.dq.1')).toHaveClass(/on/);
});
