// Selecting blocks in the editor and acting on the selection (06 §4): click, Ctrl/Shift-click,
// a left-drag marquee on the canvas (Shift/Ctrl: adding), Escape or a click on the canvas, Ctrl+A,
// Delete or Backspace, F2 (rename), and dragging the selection; a right drag pans (06 §4.0).
// Replaces Rete's selectableNodes, which moves the other selected blocks locally when the dragged
// one moves; here every move goes to the engine.

import {NodeEditor} from 'rete';
import {AreaPlugin, Drag} from 'rete-area-plugin';
import {can, isValueNode, PrimitiveNode, Schemes, vueModel} from '@/components/model';
import {setupSelection} from './index';
import {menuOnRelease, PanZoom} from './navigation';
import {BlockSelection, MoveThrottle, Position, SelectionDrag} from './blocks';
import {inputSources} from '@/components/floweditor/values';
import * as history from '@/components/floweditor/history';
import {shown} from "../shown";

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
    const publishNow = () => {
        vueModel.selectedBlocks.value = selection.ids().flatMap(id => {
            const node = editor.getNode(id);
            if (!(node instanceof PrimitiveNode))
                return [];
            return [{node: id, id: node.label, component: node.component, inputs: inputSources(editor, node)}];
        });
    };
    const publish = () => {
        if (publishing)
            return;
        publishing = true;
        queueMicrotask(() => {
            publishing = false;
            publishNow();
        });
    };
    // The process's blocks; value nodes (06 §4.3) are not selectable.
    const isBlock = (id: string) => editor.getNode(id) instanceof PrimitiveNode;

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

    // A left drag on the canvas draws the marquee, which replaces the selection (with Ctrl/Shift:
    // adds to it); a right drag, anywhere in the editor, pans, and so does a touch's.
    const pans = (e: PointerEvent) => e.pointerType === 'touch' || e.button === 2;
    area.area.setDragHandler(new Drag({down: pans, move: () => true}));
    area.area.setZoomHandler(new PanZoom(0.1, (dx, dy) => {
        const {x, y} = area.area.transform;
        void area.area.translate(x - dx, y - dy);
    }));
    const menus = menuOnRelease(area.container);
    const marquee = setupSelection(area, {
        starts: event => event.button === 0 && event.pointerType !== 'touch',
        selected: ids => {
            const blocks = ids.map(String).filter(isBlock);
            if (pressAccumulates)
                selection.add(blocks);
            else
                selection.set(blocks);
        },
    });
    marquee.setMode('center');
    marquee.setShape('marquee');

    area.addPipe(context => {
        if (!context || typeof context !== 'object' || !('type' in context))
            return context;
        switch (context.type) {
            case 'nodepicked': {
                const id = context.data.id;
                if (!isBlock(id)) {
                    drag = undefined;
                    break;
                }
                selection.press(id, pressAccumulates);
                const start = new Map<string, Position>();
                for (const selected of selection.ids()) {
                    const view = area.nodeViews.get(selected);
                    if (view !== undefined)
                        start.set(selected, {...view.position});
                }
                drag = {id, moved: false, selection: new SelectionDrag(id, start)};
                history.begin();   // the whole drag is one undo step (06 §4.13)
                break;
            }
            case 'nodetranslate':
                // Events from the engine move blocks; the user's drag only sends commands.
                if (vueModel.pipesDisabled)
                    return context;
                if (drag !== undefined && drag.id === context.data.id && can.value.edit) {
                    drag.moved = true;
                    drag.selection.positions(context.data.position)
                        .forEach((position, id) => throttle.move(id, position));
                }
                return undefined;
            case 'nodedragged':
                throttle.flush();
                if (drag !== undefined)
                    history.end();
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

    const selectAll = () => selection.set(editor.getNodes().filter(n => isBlock(n.id)).map(n => n.id));

    // Each block's removal comes back as its event, which unselects it.
    const removeSelected = () => {
        for (const id of selection.ids()) {
            const node = editor.getNode(id);
            if (node !== undefined)
                commands.remove(node.label);
        }
    };

    const keydown = (event: KeyboardEvent) => {
        if (typing(event) || !shown(area.container))
            return;
        if (event.key === 'Escape') {
            selection.clear();
        } else if (event.key === 'a' && (event.ctrlKey || event.metaKey) && !event.altKey) {
            event.preventDefault();
            selectAll();
        } else if (event.key === 'ContextMenu' || (event.key === 'F10' && event.shiftKey)) {
            event.preventDefault();
            (editor as { onMenuKey?: () => void }).onMenuKey?.();
        } else if (!can.value.edit) {
            return; // the keys below change the process (06 §4.11)
        } else if (event.key === 'F2') {
            if (selection.ids().length !== 1)
                return;
            event.preventDefault();
            vueModel.renameRequest.value++;
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
        // Selects the block alone and focuses the inspector's first input value.
        editInputs(id: string) {
            selection.set([id]);
            publishNow();
            vueModel.inputsRequest.value++;
        },
        // Selects the block alone and opens the inspector's name field.
        rename(id: string) {
            selection.set([id]);
            // Before the request, so that the inspector already shows the block it focuses.
            publishNow();
            vueModel.renameRequest.value++;
        },
        // A selected block, or what feeds its inputs, changed.
        refresh: publish,
        destroy() {
            document.removeEventListener('keydown', keydown);
            area.container.removeEventListener('pointerdown', capturePress, {capture: true});
            marquee.destroy();
            menus.destroy();
        },
    };
}
