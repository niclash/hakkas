import {describe, expect, it} from 'vitest';
import {createLogger} from '../src/log.js';

describe('the log', () => {
    it('writes a level and its message on one line', () => {
        const lines: string[] = [];
        const log = createLogger('fine', l => lines.push(l));
        log.fine('a');
        log.info('b');
        log.warn('c\nd');
        log.error('e\r\n\nf');
        expect(lines).toEqual(['fine a', 'info b', 'warn c d', 'error e f']);
    });

    it('drops what is below the threshold', () => {
        const lines: string[] = [];
        const log = createLogger('warn', l => lines.push(l));
        log.fine('a');
        log.info('b');
        log.warn('c');
        log.error('d');
        expect(lines).toEqual(['warn c', 'error d']);
    });
});
