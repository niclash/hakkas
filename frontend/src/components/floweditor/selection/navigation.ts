// Moving around the editor (06 §4.0): the wheel and a trackpad's two-finger scroll pan, a pinch or
// Ctrl+wheel zooms, and a right press opens the context menu only when released in place, since a
// right drag pans.

import {Zoom} from 'rete-area-plugin';

// A right press opens the menu when released within this many pixels of where it started.
const MENU_SLOP = 4;

// A wheel event's movement in pixels, whatever unit it came in.
function pixels(delta: number, mode: number): number {
    return mode === WheelEvent.DOM_DELTA_LINE ? delta * 40
        : mode === WheelEvent.DOM_DELTA_PAGE ? delta * 800 : delta;
}

// Rete's zoom on the wheel, a pinch of two touches and a double-click, but the wheel pans:
// a mouse's wheel and a trackpad's scroll are the same events. A pinch on a trackpad arrives as
// the wheel with Ctrl, and zooms by how far it moved (a mouse's notch: `intensity`).
export class PanZoom extends Zoom {
    constructor(intensity: number, private readonly pan: (dx: number, dy: number) => void) {
        super(intensity);
    }

    protected wheel = (e: WheelEvent) => {
        e.preventDefault();
        const dx = pixels(e.deltaX, e.deltaMode), dy = pixels(e.deltaY, e.deltaMode);
        if (e.ctrlKey || e.metaKey) {
            const {left, top} = this.element.getBoundingClientRect();
            const delta = -Math.sign(dy) * Math.min(this.intensity, Math.abs(dy) / 300);
            this.onzoom(delta, (left - e.clientX) * delta, (top - e.clientY) * delta, 'wheel');
        } else if (e.shiftKey && dx === 0) {
            this.pan(dy, 0);   // Shift turns a wheel sideways where the browser does not
        } else {
            this.pan(dx, dy);
        }
    };
}

// The context menu opens when the right button is released without having moved: browsers send
// `contextmenu` on the press (Linux, macOS) or after the release (Windows). One sent during the
// press is held back and sent again on the release; any sent after a right drag is dropped.
export function menuOnRelease(container: HTMLElement) {
    let press: { x: number, y: number, moved: boolean, held?: MouseEvent } | undefined;
    let dragged = false;     // the last right press was a drag (for a menu sent after it)
    let replaying = false;

    const move = (e: PointerEvent) => {
        if (press !== undefined && Math.hypot(e.clientX - press.x, e.clientY - press.y) > MENU_SLOP)
            press.moved = true;
    };
    const up = (e: PointerEvent) => {
        if (press === undefined || e.button !== 2)
            return;
        const {moved, held} = press;
        press = undefined;
        dragged = moved && held === undefined;   // a menu may still come (Windows)
        window.removeEventListener('pointermove', move);
        window.removeEventListener('pointerup', up);
        const target = held?.target;
        if (held === undefined || moved || !(target instanceof Element) || !target.isConnected)
            return;
        replaying = true;
        try {
            const {clientX, clientY, screenX, screenY, ctrlKey, shiftKey, altKey, metaKey} = held;
            target.dispatchEvent(new MouseEvent('contextmenu', {
                bubbles: true, cancelable: true, composed: true, button: 2,
                clientX, clientY, screenX, screenY, ctrlKey, shiftKey, altKey, metaKey,
            }));
        } finally {
            replaying = false;
        }
    };
    const down = (e: PointerEvent) => {
        dragged = false;
        if (e.button !== 2 || e.pointerType === 'touch')
            return;
        press = {x: e.clientX, y: e.clientY, moved: false};
        window.addEventListener('pointermove', move);
        window.addEventListener('pointerup', up);
    };
    const menu = (e: MouseEvent) => {
        if (replaying)
            return;
        if (press !== undefined) {
            press.held = e;
        } else if (dragged) {
            dragged = false;
        } else {
            return;   // after a release in place, or from the keyboard
        }
        e.preventDefault();
        e.stopPropagation();
    };
    container.addEventListener('pointerdown', down, {capture: true});
    container.addEventListener('contextmenu', menu, {capture: true});
    return {
        destroy() {
            container.removeEventListener('pointerdown', down, {capture: true});
            container.removeEventListener('contextmenu', menu, {capture: true});
            window.removeEventListener('pointermove', move);
            window.removeEventListener('pointerup', up);
        },
    };
}
