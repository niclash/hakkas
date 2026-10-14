// Selecting blocks in the editor and acting on the selection (06 §4): click, Ctrl/Shift-click,
// a Shift/Ctrl-drag marquee on the canvas, Escape or a click on the canvas, Ctrl+A, Delete or
// Backspace, and dragging the selection. Replaces Rete's selectableNodes, which moves the other
// selected blocks locally when the dragged one moves; here every move goes to the engine.

import {NodeEditor} from 'rete';
import {AreaPlugin, Drag} from 'rete-area-plugin';
import {Schemes} from '@/components/model';
import {vueModel} from '@/components/model';
import {setupSelection} from './index';
import {BlockSelection, MoveThrottle, Position, SelectionDrag} from './blocks';

// What the selection does to the process, by block id (the node's label).
export interface SelectionCommands {
    move(id: string, position: Position): void;
    remove(id: string): void;
}

// A pointer press counts as a click below this many pointer movements (as in Rete).
const CLICK_MOVES = 4;

function accumulates(event: { ctrlKey: boolean, shiftKey: boolean, metaKey: boolean }): boolean {
    return event.ctrlKey || event.shiftKey || event.metaKey;
}

// Keys typed into a field are the field's.
function typing(event: KeyboardEvent): boolean {
    const target = event.target as HTMLElement | null;
    return target !== null && (target.isContentEditable
        || ['INPUT', 'TEXTAREA', 'SELECT'].includes(target.tagName));
}

export function setupBlockSelection<K>(editor: NodeEditor<Schemes>, area: AreaPlugin<Schemes, K>,
                                       commands: SelectionCommands) {
    // The selected blocks' ids and types for the inspector, once per change.
    let publishing = false;
    const publish = () => {
        if (publishing)
            return;
        publishing = true;
        queueMicrotask(() => {
            publishing = false;
            vueModel.selectedBlocks.value = selection.ids().flatMap(id => {
                const node = editor.getNode(id);
                return node === undefined ? [] : [{id: node.label, component: node.component}];
            });
        });
    };
    const selection = new BlockSelection((id, selected) => {
        const node = editor.getNode(id);
        if (node !== undefined && node.selected !== selected) {
            node.selected = selected;
            void area.update('node', id);
        }
        publish();
    });
    const throttle = new MoveThrottle((id, position) => {
        const node = editor.getNode(id);
        if (node !== undefined)
            commands.move(node.label, position);
    });

    // The modifiers of the last press: a node's drag handler stops the event before the
    // container sees it, so they are taken on the way down.
    let pressAccumulates = false;
    const capturePress = (event: PointerEvent) => {
        pressAccumulates = accumulates(event);
    };
    area.container.addEventListener('pointerdown', capturePress, {capture: true});

    // A press on the canvas, until it is released: a click there clears the selection.
    let canvasPress: { moves: number } | undefined;
    // The block being dragged.
    let drag: { id: string, moved: boolean, selection: SelectionDrag } | undefined;

    // A plain drag on the canvas pans it; with Ctrl/Shift it draws the marquee.
    area.area.setDragHandler(new Drag({
        down: e => !(e.pointerType === 'mouse' && e.button !== 0) && !accumulates(e),
        move: () => true,
    }));
    const marquee = setupSelection(area, {
        starts: event => event.button === 0 && accumulates(event),
        selected: ids => selection.add(ids.map(String)),
    });
    marquee.setMode('center');
    marquee.setShape('marquee');

    area.addPipe(context => {
        if (!context || typeof context !== 'object' || !('type' in context))
            return context;
        switch (context.type) {
            case 'nodepicked': {
                const id = context.data.id;
                selection.press(id, pressAccumulates);
                const start = new Map<string, Position>();
                for (const selected of selection.ids()) {
                    const view = area.nodeViews.get(selected);
                    if (view !== undefined)
                        start.set(selected, {...view.position});
                }
                drag = {id, moved: false, selection: new SelectionDrag(id, start)};
                break;
            }
            case 'nodetranslate':
                // Events from the engine move blocks; the user's drag only sends commands.
                if (vueModel.pipesDisabled)
                    return context;
                if (drag !== undefined && drag.id === context.data.id) {
                    drag.moved = true;
                    drag.selection.positions(context.data.position)
                        .forEach((position, id) => throttle.move(id, position));
                }
                return undefined;
            case 'nodedragged':
                throttle.flush();
                selection.release(drag?.moved ?? false);
                drag = undefined;
                break;
            case 'pointerdown':
                canvasPress = context.data.event.button === 0 && !accumulates(context.data.event)
                    ? {moves: 0} : undefined;
                break;
            case 'pointermove':
                if (canvasPress !== undefined)
                    canvasPress.moves++;
                break;
            case 'pointerup':
                if (canvasPress !== undefined && canvasPress.moves < CLICK_MOVES)
                    selection.clear();
                canvasPress = undefined;
                break;
        }
        return context;
    });

    editor.addPipe(context => {
        if (context.type === 'noderemoved') {
            selection.forget(context.data.id);
            publish();
        } else if (context.type === 'cleared') {
            selection.ids().forEach(id => selection.forget(id));
            publish();
        }
        return context;
    });

    const selectAll = () => selection.set(editor.getNodes().map(n => n.id));

    // Each block's removal comes back as its event, which unselects it.
    const removeSelected = () => {
        for (const id of selection.ids()) {
            const node = editor.getNode(id);
            if (node !== undefined)
                commands.remove(node.label);
        }
    };

    const keydown = (event: KeyboardEvent) => {
        if (typing(event) || !area.container.isConnected)
            return;
        if (event.key === 'Escape') {
            selection.clear();
        } else if (event.key === 'a' && (event.ctrlKey || event.metaKey) && !event.altKey) {
            event.preventDefault();
            selectAll();
        } else if (event.key === 'Delete' || event.key === 'Backspace') {
            if (selection.ids().length === 0)
                return;
            event.preventDefault();
            removeSelected();
        }
    };
    document.addEventListener('keydown', keydown);

    return {
        selection,
        selectAll,
        removeSelected,
        // A selected block was renamed.
        refresh: publish,
        destroy() {
            document.removeEventListener('keydown', keydown);
            area.container.removeEventListener('pointerdown', capturePress, {capture: true});
            marquee.destroy();
        },
    };
}
