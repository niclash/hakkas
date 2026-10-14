// The frontend for a device's flash (06 §5): every file of dist/ stored only as <file>.gz in
// dist-device/, which the engine's web server serves with Content-Encoding: gzip
// (05 §3, "Pre-compressed assets"). Run by `npm run build:device` after the build.

import {mkdirSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync} from "node:fs";
import {dirname, join, relative} from "node:path";
import {fileURLToPath} from "node:url";
import {gzipSync} from "node:zlib";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const from = join(root, "dist"), to = join(root, "dist-device");

function files(dir) {
    return readdirSync(dir).flatMap(name => {
        const path = join(dir, name);
        return statSync(path).isDirectory() ? files(path) : [path];
    });
}

rmSync(to, {recursive: true, force: true});
let plain = 0, packed = 0;
for (const file of files(from)) {
    const data = readFileSync(file);
    const gz = gzipSync(data, {level: 9});
    const out = join(to, relative(from, file) + ".gz");
    mkdirSync(dirname(out), {recursive: true});
    writeFileSync(out, gz);
    plain += data.length;
    packed += gz.length;
}
console.log(`dist-device: ${files(to).length} files, ${(packed / 1024).toFixed(0)} KiB (from ${(plain / 1024).toFixed(0)} KiB)`);
