import {createHash, createHmac, pbkdf2Sync, randomBytes} from 'node:crypto';
import {readFileSync} from 'node:fs';
import {join} from 'node:path';
import {WebSocketServer} from 'ws';
import {expect, test} from './fixtures';

// Copying from a device (Colibri spec §5.3; plan task E16), through the UI: the emulator's
// server logs in to a device — here a fake hakkas speaking the wire protocol's login and
// environment:describe — and saves the twin under the name given. (The emulator's own tests
// copy from a real hakkas.)

const PASSWORD = 'device-pass';
const DESCRIPTION = {
    name: 'plant', outPorts: [{id: 'slot1-ai', type: 'number', addressable: true, count: 2}], inPorts: [],
    modules: [{slot: 0, builtin: true, type: 'carrier', ports: []},
        {slot: 1, type: 'colibri_aiv', serial: 'AIV-9', ports: ['slot1-ai']},
        {slot: 2, type: 'colibri_flux', ports: []}],
    carrier: {type: 'colibri_8', slots: 7, builtin: [{slot: 0}], comm: []},
};

const hmac = (key: Buffer, text: string) => createHmac('sha256', key).update(text, 'utf8').digest();

// A device's WebSocket on port + 1 (02 §0): the login (02 §2.3) for one user, and describe.
async function fakeDevice(): Promise<{address: string, close: () => void}> {
    const salt = randomBytes(16), iterations = 1024;
    const salted = pbkdf2Sync(PASSWORD, salt, iterations, 32, 'sha256');
    const storedKey = createHash('sha256').update(hmac(salted, 'Client Key')).digest();
    const serverKey = hmac(salted, 'Server Key');
    for (;;) {
        const port = 20000 + Math.floor(Math.random() * 40000);
        const wss = new WebSocketServer({port: port + 1, host: '127.0.0.1'});
        const ok = await new Promise<boolean>(r => {
            wss.once('listening', () => r(true));
            wss.once('error', () => r(false));
        });
        if (!ok)
            continue;
        wss.on('connection', ws => {
            let nonce = '';
            const send = (protocol: string, command: string, payload: object) =>
                ws.send(JSON.stringify({protocol, command, payload}));
            ws.on('message', raw => {
                const m = JSON.parse(raw.toString());
                if (m.command === 'challenge') {
                    nonce = m.payload.cnonce + randomBytes(16).toString('base64');
                    send('auth', 'challenge', {salt: salt.toString('base64'), iterations, nonce});
                } else if (m.command === 'login') {
                    const message = `${m.payload.user},${nonce},${salt.toString('base64')},${iterations}`;
                    const signature = hmac(storedKey, message);
                    const clientKey = Buffer.from(m.payload.proof, 'base64').map((b, i) => b ^ signature[i]!);
                    if (m.payload.user !== 'vera' || !createHash('sha256').update(clientKey).digest().equals(storedKey))
                        return send('network', 'error', {stack: '', message: 'Login failed'});
                    send('auth', 'login', {secret: 's3', role: 'viewer', capabilities: [],
                        server_signature: hmac(serverKey, message).toString('base64')});
                } else if (m.command === 'describe' && m.secret === 's3') {
                    send('environment', 'description', DESCRIPTION);
                }
            });
        });
        return {address: `http://127.0.0.1:${port}/`, close: () => wss.close()};
    }
}

test('copies a device into a new environment, warning about what it could not copy', async ({page, emulator}) => {
    const device = await fakeDevice();
    try {
        await page.goto(emulator.url);
        await page.getByTestId('env-copy').click();
        await page.getByTestId('copy-address').fill(device.address);
        await page.getByTestId('copy-user').fill('vera');
        await page.getByTestId('copy-password').fill('wrong');
        await page.getByTestId('env-name-field').fill('twin');
        await page.getByTestId('dialog-ok').click();
        await expect(page.getByTestId('dialog-error')).toHaveText('Login failed');

        await page.getByTestId('copy-password').fill(PASSWORD);
        await page.getByTestId('dialog-ok').click();
        await expect(page.getByTestId('dialog')).toHaveCount(0);
        await expect(page.locator('[data-testid=env-item][data-name=twin]')).toBeVisible();
        await expect(page.getByTestId('notice').first()).toContainText('Copied into twin, with 1 warning');
        await expect(page.getByTestId('notice').nth(1))
            .toContainText('slot 2: module type colibri_flux is not in the catalog');
        const file = JSON.parse(readFileSync(join(emulator.library, 'environments', 'twin.json'), 'utf8'));
        expect(file).toMatchObject({carrier: 'colibri_8', modules: [{slot: 1, type: 'colibri_aiv', serial: 'AIV-9'}]});

        // Again: the address and user are kept, the password is not.
        await page.getByTestId('env-copy').click();
        await expect(page.getByTestId('copy-address')).toHaveValue(device.address);
        await expect(page.getByTestId('copy-password')).toHaveValue('');
        await page.getByTestId('copy-password').fill(PASSWORD);
        await page.getByTestId('env-name-field').fill('twin');
        await page.getByTestId('dialog-ok').click();
        await expect(page.getByTestId('dialog-error')).toHaveText('Environment exists: twin');
        await page.getByTestId('copy-address').fill('http://127.0.0.1:1/');
        await page.getByTestId('env-name-field').fill('other');
        await page.getByTestId('dialog-ok').click();
        await expect(page.getByTestId('dialog-error')).toHaveText('Cannot connect: http://127.0.0.1:1/');
        // The password is in no log line of the emulator.
        expect(emulator.log()).not.toContain(PASSWORD);
    } finally {
        device.close();
    }
});
