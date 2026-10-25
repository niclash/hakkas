// The noise's random generator (spec §3.1, `--seed`): a small seeded generator, so a run with the
// same seed gives the same noise.

// Uniformly distributed numbers in [0, 1).
export type Random = () => number;

// Mulberry32: 32 bits of state, good enough for noise, and the same on every platform.
export function seededRandom(seed: number): Random {
    let a = seed >>> 0;
    return () => {
        a = (a + 0x6d2b79f5) >>> 0;
        let t = a;
        t = Math.imul(t ^ (t >>> 15), t | 1);
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

export function randomSeed(): number {
    return Math.floor(Math.random() * 4294967296);
}
