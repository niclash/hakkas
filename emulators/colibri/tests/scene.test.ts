import {describe, expect, it} from 'vitest';
import {loadCatalog} from '../src/catalog.js';
import {Checker} from '../src/check.js';
import {readEnvironment} from '../src/environment.js';
import {createLogger} from '../src/log.js';
import {BUILTIN_DATA} from '../src/package.js';
import {scene} from '../src/scene.js';
import {System} from '../src/system.js';

// The scene's board from the render copies (spec §8.4): the communication and MCU modules'
// pictures as decoration, each in its place.
const catalog = await loadCatalog([BUILTIN_DATA], createLogger('error', () => {}));

function system(env: object): System {
    const c = new Checker();
    const environment = readEnvironment(c, {carrier: 'colibri_8', ...env});
    expect(c.errors).toEqual([]);
    return System.build('lab', environment!, catalog, () => 0, () => 0.5).system;
}

describe('the scene\'s board', () => {
    it('has the communication and MCU modules\' pictures in their places', () => {
        const places = catalog.carriers.get('colibri_8')!.def.places!;
        const s = scene(system({comm: [{slot: 1, type: 'colibri_ethernet', ip: {mode: 'dhcp'}}]}), catalog,
            catalog.mcu.get('colibri_mcu6')!.def);
        expect(s.board!.extras).toEqual([
            {name: 'Colibri Ethernet', rect: {x: places.comm[0]!.x, y: places.comm[0]!.y, w: places.comm[0]!.w,
                h: places.comm[0]!.h}, asset: 'assets/comm/colibri_ethernet/picture.webp'},
            {name: 'Colibri MCU6', rect: places.mcu, asset: 'assets/mcu/colibri_mcu6/picture.webp'},
        ]);
    });

    it('is none without the carrier\'s copy', () => {
        const carrier = catalog.carriers.get('colibri_8')!.def;
        const copy = carrier.render!.copy;
        delete carrier.render!.copy;
        try {
            const s = scene(system({modules: [{slot: 1, type: 'colibri_aiv'}]}), catalog);
            expect(s.board).toBeNull();
            expect(s.parts[1]).not.toHaveProperty('rect');
            expect(s.rows.find(r => r.key === 'slot1.ai.1')).not.toHaveProperty('at');
        } finally {
            carrier.render!.copy = copy;
        }
    });
});
