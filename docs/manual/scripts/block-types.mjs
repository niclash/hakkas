// Generates the User's Manual's Chapter 15, the block type reference (reference/block-types.md),
// decided with the user 2026-10-27 (plan U1): from the specification, so that it cannot drift
// from it. The types and their sections come from the table of specifications/03-block-types.md
// §3 (checked against the types the engine registers); their descriptions and ports, with types
// and port descriptions, from the engine's own descriptors (`hakkas describe type`), which 03
// specifies and the C tests check.
// The notes below say what a user needs beyond that; each names a type the table has.
//
// Usage (in docs/manual): npm run reference   — needs hakkas built (HAKKAS_BIN, else the debug
// preset's build/debug/hakkas). `--check` fails instead of writing when the page is not current.
import {execFileSync} from "node:child_process";
import {readFileSync, writeFileSync} from "node:fs";

const root = new URL("../../../", import.meta.url);
const SPEC = new URL("specifications/03-block-types.md", root);
const OUT = new URL("../reference/block-types.md", import.meta.url);
const HAKKAS = process.env.HAKKAS_BIN ?? new URL("build/debug/hakkas", root).pathname;

const FAMILIES = {
    math: ["Math", "Arithmetic on numbers. Every input and output is a number."],
    compare: ["Compare", "Comparisons of two numbers, giving a bool."],
    logic: ["Logic", "Boolean logic. Every input and output is a bool; a number input is true when it is not 0."],
    process: ["Process", "The control engineer's blocks: filters, curves, limits, delays, regulators, alarms and selectors."],
    timing: ["Timing", "Pulses, times in units, and the calendar. The calendar blocks use the controller's local time (Section 16.2, `time_zone`) and wake exactly when their output may change."],
};

// What a user needs to know beyond the ports, per type.
const NOTES = {
    "math/Divide": "Division by 0 gives ±Infinity, or NaN for 0/0, as IEEE-754 arithmetic does.",
    "math/Modulo": "The floored modulo: the result has the sign of the divisor (`-7 MOD 3 = 2`). Compare `math/Remainder`.",
    "math/Remainder": "The truncating remainder: the result has the sign of the dividend (`-7 REMAINDER 3 = -1`), like C's `fmod`.",
    "math/Min": "Only the inputs that are fed (by a connection, a value, a binding or a link) count; an input fed by nothing is ignored, so 2, 3 or 4 inputs can be used in any positions. None fed gives 0. A NaN in any fed input gives NaN, so a failed sensor shows.",
    "math/Max": "As `math/Min`: only fed inputs count; none gives 0; NaN gives NaN.",
    "compare/Equal": "Exact comparison of numbers: compare measured values with `compare/Less` or `compare/Greater` and a margin instead.",
    "compare/NotEqual": "With a NaN operand every comparison is false except `NotEqual`, which is true.",
    "process/Linear": "Scaling: percent to volts (`k` = 0.1), °C to °F (`k` = 1.8, `m` = 32), a sensor's 0–10 V to its range.",
    "process/Deadband": "`out` follows `in` only when it has moved at least `threshold` from `out`. The first value after the start always passes. Use it to filter a noisy sensor more than the change threshold does, visibly in the diagram.",
    "process/PID": "Parallel form: `out = kp·e + ki·∫e dt + D`, the derivative on the measurement (no kick on a setpoint step), limited to `max_dgain`. Rests in manual (`auto` false) with output `manual`, unlimited and without sampling: set `sample_ms`, the gains and the limits. Equal limits are no limits. Switching to automatic is bumpless. The integral stops at a limit (anti-windup), so the output leaves the limit as soon as the error reverses. `reverse` for cooling. `saturated` is true at a limit. Section 12.5 explains what it needs at start-up.",
    "process/Curve": "Up to seven points; a point counts only if its `x` is greater than the last point used, so unused points resting at 0 drop out by themselves. Outside the points the output holds the nearest end's `y`, and `min` or `max` says so.",
    "process/AutoMan": "A hand switch: `override` true makes `output` follow `manual`. No bumpless transfer: the output jumps.",
    "process/Alarm": "States Normal, Activated, Deactivated (the condition gone, not acknowledged), Reactivated, Acknowledged, Disabled and Blocked; exactly one output is true, and `state` names it. `acknowledge` acts on its rising edge. Classes: A attend to at once, day and night; B during working hours; C soon; D of interest only. `block` puts a fluttering alarm aside, `disable` switches it off; Disabled wins. See Section 13.7.",
    "process/Delay": "`output` takes `input` once it has been stable for `milliseconds`; a shorter pulse never comes through. `mode` 0 delays both edges, 1 only the rising edge (an on-delay) and 2 only the falling edge (an off-delay); the other edge passes at once. At the start, mode 0 takes `input` at once; in modes 1 and 2 `output` starts false, so in mode 1 a true input waits the full time (Section 14.7). `timing/Hours` and its siblings give the time in units.",
    "process/Trigger": "A latch on edges: a rising `fire` sets `output`, a rising `arm` clears it. Levels do nothing, and an input already true at the start must go false and true again. See Section 13.3.",
    "process/Limit": "Equal limits are no limits (both rest at 0); limits the wrong way round are swapped.",
    "process/Hysteresis": "A comparator with a band: on above `threshold + hysteresis`, off below `threshold − hysteresis`, unchanged in between. `inverted` for heating (on when cold). At the start it is on only above the band.",
    "process/Exponential": "`output = a·exp(b·input)`.",
    "process/Logarithmic": "`output = a·ln(b·input)`, the natural logarithm.",
    "process/Integrator": "Adds `input × seconds / divisor` (`divisor` 3600 for hours, 86400 for days), held within the limits (equal limits: none). `reset` holds it at 0. `sample_ms` only sets how often `output` moves while `input` is constant; the result is exact either way. Running hours: integrate a bool (1 while running) with `divisor` 3600.",
    "process/SelectIn": "`output` is `in_true` while `select` is true, else `in_false`. Numbers only: a bool arrives as 0 or 1. IEC 61131-3's `SEL`.",
    "process/SelectOut": "`input` goes to `out_true` or `out_false`; the other output is 0, so nothing downstream keeps a stale value.",
    "process/MultiplexIn": "`select` is rounded; 1–8 chooses that input, anything else (0 when unfed) none, giving 0. IEC 61131-3's `MUX`, counting from 1.",
    "process/MultiplexOut": "`select` 1–8 sends `input` to that output; every other output is 0.",
    "timing/Days": "Converts days to the milliseconds that `process/Delay` and `timing/Interval` take: `2 → Days → Delay`. Fractions work.",
    "timing/Hours": "Converts hours to milliseconds: `3 → Hours → Delay`. Fractions work: 1.5 is an hour and a half.",
    "timing/Minutes": "Converts minutes to milliseconds: `10 → Minutes → Delay`. Fractions work.",
    "timing/Seconds": "Converts seconds to milliseconds: `5 → Seconds → Delay`. Fractions work: 0.25 is 250 ms.",
    "timing/Interval": "A square wave: `milliseconds` the period, `duty-cycle` the percentage of it that `out` is true. Idle until the period is set. Changes take effect at the next period, never cutting the current one.",
    "timing/MinuteRange": "`range` as `HH:MM-HH:MM`, the start included and the end excluded; it may wrap past midnight (`22:00-06:00`). All day is `00:00-24:00`; an equal start and end is never.",
    "timing/DayOfWeek": "`days` a selector of 1–7, Monday = 1 (Sunday = 1 with `sunday_first`).",
    "timing/DayOfMonth": "`days` a selector of 1–31; a day a month does not have never matches.",
    "timing/MonthOfYear": "`months` a selector of 1–12; `10-4` wraps over the new year.",
    "timing/DayOfYear": "`days` a selector of 1–366; 60 is 1 March, or 29 February in a leap year.",
    "timing/WeekOfYear": "`weeks` a selector of ISO 8601 weeks 1–53, which start on Monday.",
};

// The §3 table: | `name` | icon | description | formula |, cells split on unescaped bars.
function specTable(text) {
    const types = new Map();
    for (const line of text.split("\n")) {
        const cells = line.split(/(?<!\\)\|/).map(c => c.trim());
        const m = /^`([a-z]+\/[A-Za-z0-9]+)`$/.exec(cells[1] ?? "");
        if (m === null || types.has(m[1]))
            continue;
        const section = /§(\d+\.\d+)/.exec(cells[4] ?? "");
        types.set(m[1], {description: cells[3].replace(/\\\|/g, "|"), section: section?.[1]});
    }
    return types;
}

const spec = specTable(readFileSync(SPEC, "utf8"));
const names = execFileSync(HAKKAS, ["list", "types"], {encoding: "utf8"}).split("\n").filter(n => n !== "");
const problems = [];
for (const n of names)
    if (!spec.has(n))
        problems.push(`${n}: registered, but not in 03 §3's table`);
for (const n of spec.keys())
    if (!names.includes(n))
        problems.push(`${n}: in 03 §3's table, but not registered`);
for (const n of Object.keys(NOTES))
    if (!spec.has(n))
        problems.push(`${n}: a note for a type 03 does not have`);
if (problems.length > 0) {
    console.error(problems.join("\n"));
    process.exit(1);
}

const cell = s => s.replace(/\|/g, "\\|").replace(/\n/g, " ");
const out = [];
out.push("# 15. Block type reference", "",
    "<!-- Generated by docs/manual/scripts/block-types.mjs (npm run reference) from",
    "     specifications/03-block-types.md and the engine's descriptors. Do not edit by hand. -->", "",
    "Every block type built into hakkas, by family. For each: what it does, its ports, and what to",
    "know when using it. The ports are as the controller describes them; an input fed by nothing",
    "rests at 0, false or an empty text (Section 2.3). The section numbers refer to the",
    "specification, `specifications/03-block-types.md`, which has every detail.", "",
    "Composite types, such as the standard library's `schedule/WeekSchedule`, are not listed here:",
    "they are described where they come from (Chapter 7).", "");
for (const [family, [title, intro]] of Object.entries(FAMILIES)) {
    const members = names.filter(n => n.startsWith(family + "/"));
    out.push(`## ${title}`, "", intro, "");
    for (const name of members) {
        const d = JSON.parse(execFileSync(HAKKAS, ["describe", "type", name], {encoding: "utf8"}));
        const s = spec.get(name);
        out.push(`### \`${name}\``, "", d.description + (s.section ? ` (03 §${s.section})` : ""), "");
        out.push("| Port | | Type | Description |", "|---|---|---|---|");
        for (const p of d.inPorts)
            out.push(`| \`${p.id}\` | in | ${p.type} | ${cell(p.description)} |`);
        for (const p of d.outPorts)
            out.push(`| \`${p.id}\` | out | ${p.type} | ${cell(p.description)} |`);
        out.push("");
        if (NOTES[name])
            out.push(NOTES[name], "");
    }
}
const text = out.join("\n");
if (process.argv.includes("--check")) {
    if (readFileSync(OUT, "utf8") !== text) {
        console.error("reference/block-types.md is not current: npm run reference");
        process.exit(1);
    }
} else {
    writeFileSync(OUT, text);
    console.log(`reference/block-types.md: ${names.length} block types`);
}
