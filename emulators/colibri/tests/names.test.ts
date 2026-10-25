import {describe, expect, it} from 'vitest';
import {isEnvironmentName} from '../src/names.js';

describe('environment names', () => {
    it('are 1-64 letters, digits, dots, underscores and hyphens', () => {
        for (const name of ['a', 'lab', 'Pump-Room_2', 'v1.2', '-x', '_', 'x'.repeat(64)])
            expect(isEnvironmentName(name), name).toBe(true);
    });

    it('do not start with a dot or hold anything else', () => {
        for (const name of ['', '.', '.lab', 'a/b', 'a b', 'å', 'x'.repeat(65), 'a\n'])
            expect(isEnvironmentName(name), name).toBe(false);
    });
});
