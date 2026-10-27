// Writes the description of an emulator environment file (examples/<x>/<env>.json) as the
// Colibri emulator sends it to hakkas, for the manual's screenshots, which serve it from the
// test fixtures' fake environment program. Needs the emulator built (npm run build in
// emulators/colibri). Usage: node describe-environment.mjs <environment file> <name> <out>
import {readFileSync, writeFileSync} from "node:fs";
const dist = new URL("../../../emulators/colibri/dist/", import.meta.url);
const {loadCatalog} = await import(new URL("catalog.js", dist));
const {Checker} = await import(new URL("check.js", dist));
const {readEnvironment} = await import(new URL("environment.js", dist));
const {describe} = await import(new URL("description.js", dist));
const {createLogger} = await import(new URL("log.js", dist));
const {BUILTIN_DATA} = await import(new URL("package.js", dist));
const {System} = await import(new URL("system.js", dist));

const [file, name, out] = process.argv.slice(2);
const catalog = await loadCatalog([BUILTIN_DATA], createLogger("error", () => {}));
const c = new Checker();
const env = readEnvironment(c, JSON.parse(readFileSync(file, "utf8")));
if (c.errors.length > 0)
    throw new Error(c.errors.join("\n"));
const {system} = System.build(name, env, catalog, () => 0, Math.random);
writeFileSync(out, JSON.stringify(describe(system), null, 2) + "\n");
