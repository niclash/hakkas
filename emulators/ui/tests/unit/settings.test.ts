import {describe, expect, it} from 'vitest';
import {countersOf, groupFields, listText, optionKey, parseList} from '@/settings';
import type {Field, Scene, Stats} from '@/protocol/types';

describe('the settings form', () => {
    it('puts the fields without a group first, then each group in the order it comes', () => {
        const f = (id: string, group?: string): Field => ({id, label: id, type: 'text', ...(group && {group})});
        const groups = groupFields([f('carrier'), f('slot0', 'Slots'), f('cycle'), f('comm1', 'Communication'), f('slot1', 'Slots')]);
        expect(groups.map(g => [g.title, g.fields.map(x => x.id)])).toEqual([
            ['', ['carrier', 'cycle']], ['Slots', ['slot0', 'slot1']], ['Communication', ['comm1']]]);
        expect(groupFields([f('a', 'G')]).map(g => g.title)).toEqual(['G']);
    });

    it('shows a list as comma-separated text, and reads it back', () => {
        expect(listText(['1.1.1.1', '9.9.9.9'])).toBe('1.1.1.1, 9.9.9.9');
        expect(listText(null)).toBe('');
        expect(parseList(' 1.1.1.1,, 9.9.9.9 ,')).toEqual(['1.1.1.1', '9.9.9.9']);
        expect(parseList('')).toEqual([]);
    });

    it('keys options of any type', () => {
        expect(optionKey(9600)).toBe('9600');
        expect(optionKey('9600')).toBe('"9600"');
        expect(JSON.parse(optionKey(''))).toBe('');
    });
});

describe('the counters', () => {
    it('follow the scene: filled parts with their channels, outputs and inputs apart', () => {
        const scene: Scene = {board: null,
            parts: [{id: 'slot1', place: 'slot-1', type: 'colibri_aiv', name: 'Colibri AIV'},
                {id: 'slot2', place: 'slot-2', type: null, name: null},
                {id: 'slot3', place: 'slot-3', type: 'colibri_ssr', name: 'Colibri SSR'}],
            rows: [{key: 'slot1.ai.1', part: 'slot1', label: 'slot1-ai[1]', kind: 'input', type: 'number'},
                {key: 'slot3.dq.1', part: 'slot3', label: 'slot3-dq[1]', kind: 'output', type: 'bool'}]};
        const stats: Stats = {parts: {slot1: {queue: 0, queueMax: 0, dropped: 0}, slot3: {queue: 1, queueMax: 4, dropped: 2}},
            channels: {'slot1.ai.1': {sent: 7}, 'slot3.dq.1': {received: 6, queued: 4, dropped: 2}}};
        const c = countersOf(stats, scene);
        expect(c.parts).toEqual([{id: 'slot1', name: 'Colibri AIV', queue: 0, queueMax: 0, dropped: 0},
            {id: 'slot3', name: 'Colibri SSR', queue: 1, queueMax: 4, dropped: 2}]);
        expect(c.channels).toEqual([
            {key: 'slot1.ai.1', label: 'slot1-ai[1]', part: 'slot1', output: false, sent: 7},
            {key: 'slot3.dq.1', label: 'slot3-dq[1]', part: 'slot3', output: true, received: 6, queued: 4, dropped: 2}]);
    });
});
