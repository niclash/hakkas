import {describe, expect, it} from 'vitest';
import {Checker, type Json, type JsonObject} from '../src/check.js';
import {readCarrier, readComm, readMcu, readModule, type CarrierType, type ModuleType} from '../src/descriptors.js';

function module(v: JsonObject, dir = v.type as string): {def?: ModuleType, errors: string[], warnings: string[]} {
    const c = new Checker();
    const def = readModule(c, v, dir);
    return {def, errors: c.errors, warnings: c.warnings};
}

function moduleErrors(channels: Json[], extra: JsonObject = {}): string[] {
    const r = module({type: 'm', channels, ...extra});
    expect(r.def).toBeUndefined();
    return r.errors;
}

const ai = {id: 'ai', direction: 'input', type: 'number', count: 2, unit: 'V', min: 0, max: 10, resolution: 0.001};

describe('module descriptors', () => {
    it('read a module with its defaults filled in', () => {
        const r = module({type: 'colibri_x', name: 'Colibri X', vendor: 'V', revision: '1', channels: [
            ai,
            {id: 'fault', direction: 'input', type: 'bool'},
            {id: 'dq', direction: 'output', type: 'bool', count: 4},
            {id: 'aq', direction: 'output', type: 'number', min: -5, max: -1, indicator: 'bar'},
            {id: 'n', direction: 'input', type: 'number', min: 2, max: 3},
            {id: 'c', direction: 'input', type: 'number', resolution: 1},
        ]});
        expect(r.errors).toEqual([]);
        expect(r.warnings).toEqual([]);
        expect(r.def).toEqual({
            type: 'colibri_x', name: 'Colibri X', vendor: 'V', revision: '1', signals: [], positions: [], bindings: [],
            controls: [],
            channels: [
                {...ai, default: {kind: 'constant', value: 0}, noise: 0, control: 'slider',
                    sources: ['constant', 'sine', 'square', 'ramp', 'sequence']},
                {id: 'fault', direction: 'input', type: 'bool', count: 1, default: {kind: 'constant', value: false},
                    noise: 0, control: 'toggle', sources: ['constant', 'square', 'sequence']},
                {id: 'dq', direction: 'output', type: 'bool', count: 4, default: false, indicator: 'lamp'},
                {id: 'aq', direction: 'output', type: 'number', count: 1, min: -5, max: -1, default: 0, indicator: 'bar'},
                {id: 'n', direction: 'input', type: 'number', count: 1, min: 2, max: 3,
                    default: {kind: 'constant', value: 2}, noise: 0, control: 'slider',
                    sources: ['constant', 'sine', 'square', 'ramp', 'sequence']},
                {id: 'c', direction: 'input', type: 'number', count: 1, resolution: 1,
                    default: {kind: 'constant', value: 0}, noise: 0, control: 'number',
                    sources: ['constant', 'sine', 'square', 'ramp', 'counter', 'sequence']},
            ],
        });
    });

    it('check the type name against its directory', () => {
        expect(module({type: 'a'}, 'b').errors).toEqual(["type: differs from its directory's name, b"]);
        expect(module({type: 'Colibri/AIV'}, 'x').errors[0]).toMatch(/^type: expected 1 to 64 lower-case/);
        expect(module({}, 'x').errors).toEqual(['type: missing']);
        expect(module({type: 'carrier', channels: []}).def).toEqual(
            {type: 'carrier', channels: [], signals: [], positions: [], bindings: [], controls: []});
    });

    it('refuse bad channels', () => {
        expect(moduleErrors([{...ai, id: 'AI'}])).toEqual(['channels[0].id: expected 1 to 32 lower-case letters, digits and _']);
        expect(moduleErrors([ai, ai])).toEqual(['channels[1]: ai is defined twice']);
        expect(moduleErrors([{...ai, direction: 'in'}])).toEqual(['channels[0].direction: expected one of input, output']);
        expect(moduleErrors([{...ai, count: 0}])).toEqual(['channels[0].count: expected an integer from 1 to 1024']);
        expect(moduleErrors([{...ai, min: 11}])).toEqual(['channels[0].min: greater than max']);
        expect(moduleErrors([{id: 'b', direction: 'input', type: 'bool', min: 0}]))
            .toEqual(['channels[0].min: not for a bool channel']);
        expect(moduleErrors([{...ai, resolution: 0}])).toEqual(['channels[0].resolution: expected a number > 0']);
        expect(moduleErrors([{...ai, noise: -1}])).toEqual(['channels[0].noise: expected a number ≥ 0']);
        expect(moduleErrors([{...ai, default: true}])).toEqual(['channels[0].default: expected a number']);
        expect(moduleErrors([{...ai, default: {counter: {rate: 1}}}])[0]).toMatch(/^channels\[0\].default: source counter not accepted/);
        expect(moduleErrors([{...ai, control: 'toggle'}])).toEqual(['channels[0].control: expected one of slider, number']);
        expect(moduleErrors([{id: 'x', direction: 'input', type: 'number', control: 'slider'}]))
            .toEqual(['channels[0].control: a slider needs min and max']);
        expect(moduleErrors([{id: 'q', direction: 'output', type: 'number', indicator: 'meter'}]))
            .toEqual(['channels[0].indicator: meter needs min and max']);
        expect(moduleErrors([{id: 'q', direction: 'output', type: 'number', noise: 1, control: 'number'}]))
            .toEqual(['channels[0].noise: not for an output', 'channels[0].control: not for an output']);
        expect(moduleErrors([{id: 'q', direction: 'output', type: 'bool', default: 1}]))
            .toEqual(['channels[0].default: expected true or false']);
        expect(moduleErrors([5])).toEqual(['channels[0]: expected an object']);
        expect(moduleErrors([], {channels: {}})).toEqual(['channels: expected an array']);
    });

    it('read signals and the channels measured from them', () => {
        const signals = [{id: 'in', type: 'pulse', count: 2, default: {pulse: {frequency: 5, duty: 50}}}];
        const r = module({type: 'm', signals, channels: [
            {id: 'di', direction: 'input', type: 'bool', count: 2, measure: {signal: 'in', quantity: 'state'}},
            {id: 'count', direction: 'input', type: 'number', count: 2, resolution: 1,
                measure: {signal: 'in', quantity: 'count'}},
        ]});
        expect(r.errors).toEqual([]);
        expect(r.def!.signals).toEqual([{id: 'in', type: 'pulse', count: 2,
            default: {kind: 'pulse', frequency: {kind: 'constant', value: 5}, duty: {kind: 'constant', value: 50}}}]);
        expect(r.def!.channels[0]).toEqual({id: 'di', direction: 'input', type: 'bool', count: 2,
            measure: {signal: 'in', quantity: 'state'}, noise: 0});
        expect(module({type: 'm', signals: [{id: 'in', type: 'pulse'}]}).def!.signals[0]!.default).toEqual(
            {kind: 'pulse', frequency: {kind: 'constant', value: 0}, duty: {kind: 'constant', value: 0}});
    });

    it('refuse bad signals and measures', () => {
        const signals = [{id: 'in', type: 'pulse', count: 2}];
        const measured = (m: Json, extra: JsonObject = {}) =>
            moduleErrors([{id: 'f', direction: 'input', type: 'number', count: 2, measure: m, ...extra}], {signals});
        expect(measured({signal: 'nope', quantity: 'frequency'}))
            .toEqual(['channels[0].measure.signal: no signal nope (signals come before the channels measuring them)']);
        expect(measured({signal: 'in', quantity: 'state'})).toEqual(['channels[0].measure.quantity: state is a bool']);
        expect(measured({signal: 'in', quantity: 'frequency'}, {count: 3}))
            .toEqual(["channels[0].count: differs from signal in's count, 2"]);
        expect(measured({signal: 'in', quantity: 'speed'}))
            .toEqual(['channels[0].measure.quantity: expected one of state, frequency, duty, count']);
        expect(measured({signal: 'in', quantity: 'duty'}, {default: 3}))
            .toEqual(['channels[0].default: not for a measured input']);
        expect(moduleErrors([{id: 'in', direction: 'input', type: 'bool'}], {signals}))
            .toEqual(['channels[0]: in is defined twice']);
        expect(moduleErrors([], {signals: [{id: 'in', type: 'square'}]})).toEqual(['signals[0].type: expected one of pulse']);
        expect(moduleErrors([], {signals: [{id: 'in', type: 'pulse', default: 3}]})[0])
            .toMatch(/^signals\[0\].default: source constant not accepted/);
    });

    it('check pictures and their keys', () => {
        const ok = module({type: 'm', faceplate: 'face.svg', channels: [ai, {id: 'dq', direction: 'output', type: 'bool'}],
            ui: {
                bindings: [{element: 'led', attr: 'fill', key: 'dq', map: {true: 'red'}},
                    {element: 'v1', text: 'ai.1', format: '%.2f V'}, {element: 'n', text: 'ai.2.noise'}],
                controls: [{element: 't1', control: 'slider', key: 'ai.2.source', min: 0, max: 10}],
            }});
        expect(ok.errors).toEqual([]);
        expect(ok.def!.faceplate).toBe('face.svg');
        expect(ok.def!.bindings[1]).toEqual({element: 'v1', text: 'ai.1', format: '%.2f V', key: 'ai.1'});
        expect(ok.def!.controls).toHaveLength(1);

        const bad = (ui: JsonObject) => moduleErrors([ai, {id: 'dq', direction: 'output', type: 'bool'}], {ui});
        for (const key of ['ai', 'ai.3', 'ai.0', 'ai.01', 'dq.1', 'xx', 'dq.noise', 'ai.1.x.source'])
            expect(bad({bindings: [{element: 'e', key}]})).toEqual([`ui.bindings[0].key: no such key in this module: ${key}`]);
        expect(bad({bindings: [{element: 'e'}]})).toEqual(['ui.bindings[0]: expected key, text or visible']);
        expect(bad({controls: [{element: 'e', control: 'slider', key: 'ai.1'}]}))
            .toEqual(['ui.controls[0].key: a control sets a source: expected a key ending in .source']);
        expect(bad({controls: [{element: 'e', control: 'knob', key: 'ai.1.source'}]}))
            .toEqual(['ui.controls[0].control: expected one of slider, number, toggle, button']);
        expect(bad({controls: [{element: 'e', control: 'toggle', key: 'dq.source'}]}))
            .toEqual(['ui.controls[0].key: no such key in this module: dq.source']);
        expect(moduleErrors([], {faceplate: '../x.svg'})[0]).toMatch(/^faceplate: expected a picture file name/);
        expect(moduleErrors([], {faceplate: 'x.exe'})[0]).toMatch(/^faceplate: expected a picture file name/);
    });

    it('read the picture link and where the channels are on the picture', () => {
        const dq = {id: 'dq', direction: 'output', type: 'bool'};
        const render = {url: 'https://example.com/m.png', rotation: 90, crop: {x: 0.2, y: 0.25, w: 0.55, h: 0.5}};
        const ok = module({type: 'm', render, channels: [ai, dq],
            positions: [{channel: 'ai', index: 2, x: 0.7, y: 0.4}, {channel: 'dq', x: 0, y: 1}]});
        expect(ok.errors).toEqual([]);
        expect(ok.def!.render).toEqual(render);
        expect(module({type: 'm', render: {url: 'https://x/m.png', rotation: 0}}).def!.render)
            .toEqual({url: 'https://x/m.png'});
        expect(ok.def!.positions).toEqual([{channel: 'ai', index: 2, x: 0.7, y: 0.4}, {channel: 'dq', x: 0, y: 1}]);

        // Only https; a quarter turn; a crop inside the file.
        expect(moduleErrors([], {render: {url: 'http://example.com/m.webp'}}))
            .toEqual(['render.url: expected an https link']);
        expect(moduleErrors([], {render: {url: 'm.webp'}})).toEqual(['render.url: expected an https link']);
        expect(moduleErrors([], {render: {}})).toEqual(['render: expected url, copy or both']);
        // The device's own copy (hakkas plan task 77): a picture file beside the descriptor.
        expect(module({type: 'm', render: {copy: 'picture.webp'}}).def!.render).toEqual({copy: 'picture.webp'});
        expect(moduleErrors([], {render: {url: 'https://x/m.png', copy: '../x.webp'}}))
            .toEqual(['render.copy: expected a picture file name in the same directory (.svg, .png, .jpg, .webp)']);
        expect(moduleErrors([], {render: {url: 'https://x/m.png', rotation: 45}}))
            .toEqual(['render.rotation: expected 0, 90, 180 or 270']);
        expect(moduleErrors([], {render: {url: 'https://x/m.png', rotation: 360}}))
            .toEqual(['render.rotation: expected an integer from 0 to 270']);
        expect(moduleErrors([], {render: {url: 'https://x/m.png', crop: {x: 0.5, y: 0, w: 0.6, h: 1}}}))
            .toEqual(['render.crop: reaches outside the picture']);
        const bad = (positions: Json[]) => moduleErrors([ai, dq], {positions});
        expect(bad([{channel: 'xx', x: 0, y: 0}])).toEqual(['positions[0].channel: no channel xx in this module']);
        expect(bad([{channel: 'ai', x: 0, y: 0}])).toEqual(['positions[0].index: missing']);
        expect(bad([{channel: 'ai', index: 3, x: 0, y: 0}]))
            .toEqual(['positions[0].index: expected an integer from 1 to 2']);
        expect(bad([{channel: 'dq', index: 1, x: 0, y: 0}])).toEqual(['positions[0].index: channel dq is not a group']);
        expect(bad([{channel: 'dq', x: 1.2, y: 0}])).toEqual(['positions[0].x: expected a number from 0 to 1']);
        expect(bad([{channel: 'dq', x: 0}])).toEqual(['positions[0].y: missing']);
        expect(bad([{channel: 'dq', x: 0, y: 0}, {channel: 'dq', x: 1, y: 1}]))
            .toEqual(['positions[1]: dq is placed twice']);
    });

    it('warn about unknown members', () => {
        const r = module({type: 'm', colour: 'red', channels: [{...ai, tolerance: 1}]});
        expect(r.errors).toEqual([]);
        expect(r.warnings).toEqual(['colour: unknown member, ignored', 'channels[0].tolerance: unknown member, ignored']);
    });

    it('refuse metadata strings longer than hakkas takes', () => {
        expect(moduleErrors([], {name: 'x'.repeat(1025)})).toEqual(['name: longer than 1024 bytes']);
    });
});

function carrier(v: JsonObject): {def?: CarrierType, errors: string[]} {
    const c = new Checker();
    const def = readCarrier(c, {type: 'c', ...v}, 'c');
    return {def, errors: c.errors};
}

describe('carrier descriptors', () => {
    it('read a carrier', () => {
        const r = carrier({name: 'C', slots: 7, builtin: [{slot: 0, type: 'carrier'}],
            power: {voltage: {min: 7, max: 30}, ac: true, dc: true, current: 2},
            comm: [{slot: 1, types: ['colibri_ethernet']}], picture: 'board.svg'});
        expect(r.errors).toEqual([]);
        expect(r.def).toEqual({type: 'c', name: 'C', slots: 7, builtin: [{slot: 0, type: 'carrier'}],
            power: {voltage: {min: 7, max: 30}, ac: true, dc: true, current: 2},
            comm: [{slot: 1, types: ['colibri_ethernet']}], picture: 'board.svg'});
    });

    it('keep to hakkas’ slot limits', () => {
        expect(carrier({}).errors).toEqual(['slots: missing', 'slots: expected 1 to 31 I/O slots, built-in ones included']);
        expect(carrier({slots: 0}).errors).toEqual(['slots: expected 1 to 31 I/O slots, built-in ones included']);
        expect(carrier({slots: 0, builtin: [{slot: 0, type: 'carrier'}]}).errors).toEqual([]);
        expect(carrier({slots: 31}).errors).toEqual([]);
        expect(carrier({slots: 31, builtin: [{slot: 0, type: 'x'}]}).errors[0]).toMatch(/expected 1 to 31 I\/O slots/);
        expect(carrier({slots: 2, builtin: [{slot: 3, type: 'x'}]}).errors)
            .toEqual(['builtin[0].slot: expected an integer from 0 to 2']);
        expect(carrier({slots: 2, builtin: [{slot: 0, type: 'x'}, {slot: 0, type: 'y'}]}).errors)
            .toEqual(['builtin[1].slot: slot 0 is built in twice']);
        const comm = (n: number) => Array.from({length: n}, (_, i) => ({slot: i + 1, types: ['t']}));
        expect(carrier({slots: 1, comm: comm(4)}).errors).toEqual([]);
        expect(carrier({slots: 1, comm: comm(5)}).errors[0]).toEqual('comm: more than 4 communication slots');
        expect(carrier({slots: 1, comm: [{slot: 1, types: ['t']}, {slot: 1, types: ['u']}]}).errors)
            .toEqual(['comm[1].slot: communication slot 1 is listed twice']);
        expect(carrier({slots: 1, comm: [{slot: 1, types: []}]}).errors)
            .toEqual(['comm[0].types: expected at least one communication module type']);
        expect(carrier({slots: 1, power: {current: '2 A'}}).errors).toEqual(['power.current: expected a number']);
        expect(carrier({slots: 1, mcu: {types: []}}).errors).toEqual(['mcu.types: expected at least one MCU module type']);
        expect(carrier({slots: 1, mcu: {types: ['MCU6']}}).errors).toEqual(['mcu.types[0]: expected a type name']);
    });

    it('read where the slots are on the picture', () => {
        const rect = {x: 0.1, y: 0.4, w: 0.05, h: 0.2};
        const base = {slots: 7, builtin: [{slot: 0, type: 'carrier'}], comm: [{slot: 1, types: ['t']}],
            mcu: {types: ['m']}, render: {url: 'https://example.com/c.webp'}};
        const r = carrier({...base, places: {slots: [{slot: 1, ...rect}, {slot: 7, ...rect}],
            comm: [{slot: 1, ...rect}], mcu: rect}});
        expect(r.errors).toEqual([]);
        expect(r.def!.render).toEqual({url: 'https://example.com/c.webp'});
        expect(r.def!.places).toEqual({slots: [{slot: 1, ...rect}, {slot: 7, ...rect}], comm: [{slot: 1, ...rect}],
            mcu: rect});

        const bad = (places: JsonObject) => carrier({...base, places}).errors;
        expect(bad({slots: [{slot: 8, ...rect}]})).toEqual(['places.slots[0].slot: expected an integer from 0 to 7']);
        expect(bad({slots: [{slot: 1, ...rect}, {slot: 1, ...rect}]}))
            .toEqual(['places.slots[1].slot: slot 1 is placed twice']);
        expect(bad({comm: [{slot: 2, ...rect}]}))
            .toEqual(['places.comm[0].slot: the carrier has no communication slot 2']);
        expect(bad({slots: [{slot: 1, ...rect, x: 0.97}]})).toEqual(['places.slots[0]: reaches outside the picture']);
        expect(bad({slots: [{slot: 1, ...rect, h: -0.1}]}))
            .toEqual(['places.slots[0].h: expected a number from 0 to 1']);
        const {mcu: _, ...noMcu} = base;
        expect(carrier({...noMcu, places: {mcu: rect}}).errors).toEqual(['places.mcu: the carrier takes no MCU module']);
    });
});

describe('MCU module descriptors', () => {
    it('read their metadata', () => {
        const c = new Checker();
        expect(readMcu(c, {type: 'colibri_mcu6', name: 'Colibri MCU6', description: 'STM32H735RG'}, 'colibri_mcu6'))
            .toEqual({type: 'colibri_mcu6', name: 'Colibri MCU6', description: 'STM32H735RG'});
        expect(c.errors).toEqual([]);
        const bad = new Checker();
        readMcu(bad, {type: 'colibri_mcu6', flash: 16}, 'colibri_mcu6');
        expect(bad.errors.length + bad.warnings.length).toBeGreaterThan(0);
    });
});

describe('communication module descriptors', () => {
    it('read their parameter groups', () => {
        const c = new Checker();
        expect(readComm(c, {type: 'w', name: 'W', params: ['ip', 'wifi']}, 'w'))
            .toEqual({type: 'w', name: 'W', params: ['ip', 'wifi']});
        expect(c.errors).toEqual([]);
        const bad = new Checker();
        expect(readComm(bad, {type: 'w', params: ['ip', 'ip', 'bluetooth']}, 'w')).toBeUndefined();
        expect(bad.errors).toEqual(['params[1]: ip is listed twice', 'params[2]: expected one of ip, wifi, rs485, lorawan']);
    });
});
