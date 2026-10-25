// Writing files so that a crash or a full disk never leaves half of one: the data goes to a
// temporary file in the same directory, is synced, and then takes the file's place.
import {randomBytes} from 'node:crypto';
import {link, open, rename, unlink} from 'node:fs/promises';
import path from 'node:path';

// Writes `data` to `file`, replacing it; with `exclusive`, an existing file is not replaced and
// the call fails with EEXIST.
export async function writeFileAtomic(file: string, data: string, exclusive = false): Promise<void> {
    const dir = path.dirname(file);
    const tmp = path.join(dir, `.${path.basename(file)}.${randomBytes(4).toString('hex')}.tmp`);
    const handle = await open(tmp, 'wx', 0o644);
    try {
        await handle.writeFile(data);
        await handle.sync();
    } catch (e) {
        await handle.close();
        await unlink(tmp).catch(() => {});
        throw e;
    }
    await handle.close();
    if (exclusive) {
        try {
            await link(tmp, file);     // fails when `file` exists
        } finally {
            await unlink(tmp);
        }
    } else {
        try {
            await rename(tmp, file);
        } catch (e) {
            await unlink(tmp).catch(() => {});
            throw e;
        }
    }
    await syncDir(dir);
}

// Renames `from` to `to` without replacing an existing `to` (EEXIST).
export async function renameExclusive(from: string, to: string): Promise<void> {
    await link(from, to);
    await unlink(from);
    await syncDir(path.dirname(to));
}

async function syncDir(dir: string): Promise<void> {
    const handle = await open(dir, 'r');
    try {
        await handle.sync();
    } finally {
        await handle.close();
    }
}
