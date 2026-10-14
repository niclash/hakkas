import {NodeEditor} from 'rete';

import {Area2D, AreaExtensions, AreaPlugin} from 'rete-area-plugin';
import {ConnectionPlugin, Presets as ConnectionPresets, SocketData} from 'rete-connection-plugin';

import {Presets as VuePresets, VueArea2D, VuePlugin} from 'rete-vue-plugin';

import {AutoArrangePlugin, Presets as ArrangePresets,} from 'rete-auto-arrange-plugin';

import {ContextMenuExtra, ContextMenuPlugin, Presets as ContextMenuPresets,} from 'rete-context-menu-plugin';
import {MinimapExtra, MinimapPlugin} from 'rete-minimap-plugin';
import {RerouteExtensions, RerouteExtra, ReroutePlugin,} from 'rete-connection-reroute-plugin';
import {isValueNode, Schemes, vueModel} from "@/components/model";
import ValueNodeView from "@/components/floweditor/ValueNodeView.vue";
import ValueConnectionView from "@/components/floweditor/ValueConnectionView.vue";
import {Position} from "rete-area-plugin/_types/types";
import {setupBlockSelection} from "@/components/floweditor/selection/editor";

type AreaExtra =
    | Area2D<Schemes>
    | VueArea2D<Schemes>
    | ContextMenuExtra
    | MinimapExtra
    | RerouteExtra;

export class FlowEditor extends NodeEditor<Schemes> {
    area: AreaPlugin<Schemes, AreaExtra>;
    blockSelection: ReturnType<typeof setupBlockSelection> | undefined;
    // A connection is being picked up to be dragged elsewhere (until it is removed).
    picking = false;
    // A line was dropped: on nothing (`socket` null) or on a socket; `created` when that made a
    // connection. `initial` is the socket it was drawn from (06 §4.3).
    onDrop: ((initial: SocketData, socket: SocketData | null, created: boolean) => void) | undefined;

    constructor(area: AreaPlugin<Schemes, AreaExtra>) {
        super();
        this.area = area;
    }

    setPosition(id: string, {x, y}: Position): Promise<boolean | undefined> {
        let xy: Position = {x, y};
        return this.area.translate(id, xy);
    }
}

export let editor: FlowEditor;

type MenuItems = ReturnType<typeof ContextMenuPresets.classic.setup<Schemes>>;

// A block's menu gets Rename (06 §4.2), which opens the inspector's name field.
function withRename(items: MenuItems): MenuItems {
    return (context, plugin) => {
        const collection = items(context, plugin);
        if (context === 'root' || !('label' in context) || isValueNode(context))
            return collection;
        return {
            ...collection,
            list: [{label: 'Rename', key: 'rename', handler: () => editor.blockSelection?.rename(context.id)},
                ...collection.list],
        };
    };
}

export async function createEditor(container: HTMLElement) {
    const area = new AreaPlugin<Schemes, AreaExtra>(container);
    editor = new FlowEditor(area);
    const connection = new ConnectionPlugin<Schemes, AreaExtra>();
    const vueRender = new VuePlugin<Schemes, AreaExtra>();
    const minimap = new MinimapPlugin<Schemes>();
    const reroutePlugin = new ReroutePlugin<Schemes>();

    editor.use(area);

    const arrange = new AutoArrangePlugin<Schemes>();
    arrange.addPreset(ArrangePresets.classic.setup());
    const contextMenu = new ContextMenuPlugin<Schemes>({
        items: withRename(ContextMenuPresets.classic.setup([
            ['Select All', () => {
                editor.blockSelection?.selectAll();
                return editor.getNodes()[0];
            }],
            ['Layout All', () => {
                arrange.layout();
                return editor.getNodes()[0];
            }],
            ['Zoom To Fit', () => {
                AreaExtensions.zoomAt(area, editor.getNodes());
                return editor.getNodes()[0];
            }],
        ])),
    });
    area.use(vueRender);
    area.use(connection);
    area.use(contextMenu);
    area.use(minimap);

    vueRender.use(reroutePlugin);

    connection.addPreset(ConnectionPresets.classic.setup());
    connection.addPipe(context => {
        if (context.type === 'connectionpick') {
            editor.picking = true;
        } else if (context.type === 'connectiondrop') {
            editor.picking = false;
            editor.onDrop?.(context.data.initial, context.data.socket, context.data.created);
        }
        return context;
    });

    vueRender.addPreset(VuePresets.classic.setup({
        customize: {
            // Fixed values are drawn as value nodes (06 §4.3).
            node(context) {
                return isValueNode(context.payload) ? ValueNodeView : VuePresets.classic.Node;
            },
            connection(context) {
                return isValueNode(editor.getNode(context.payload.source))
                    ? ValueConnectionView : VuePresets.classic.Connection;
            },
        },
    }));
    vueRender.addPreset(VuePresets.contextMenu.setup());
    vueRender.addPreset(VuePresets.minimap.setup());
    vueRender.addPreset(
        VuePresets.reroute.setup({
            contextMenu(id) {
                reroutePlugin.remove(id);
            },
            translate(id, dx, dy) {
                reroutePlugin.translate(id, dx, dy);
            },
            pointerdown(id) {
                reroutePlugin.unselect(id);
                reroutePlugin.select(id);
            },
        })
    );
    area.use(arrange);
    await arrange.layout();
    await AreaExtensions.zoomAt(area, editor.getNodes());
    editor.blockSelection = setupBlockSelection(editor, area, {
        move: (id, position) => vueModel.connection.value?.proto.graph.request_changenode(id, position),
        remove: id => vueModel.connection.value?.proto.graph.request_removenode(id),
    });
    // Reroute pins are the view's own, not the engine's.
    RerouteExtensions.selectablePins(reroutePlugin, AreaExtensions.selector(), AreaExtensions.accumulateOnCtrl());
    return {
        destroy: () => {
            editor.blockSelection?.destroy();
            area.destroy();
        },
    };
}
