import {NodeEditor} from 'rete';

import {Area2D, AreaExtensions, AreaPlugin} from 'rete-area-plugin';
import {ConnectionPlugin, Presets as ConnectionPresets, SocketData} from 'rete-connection-plugin';

import {Presets as VuePresets, VueArea2D, VuePlugin} from 'rete-vue-plugin';

import {AutoArrangePlugin, Presets as ArrangePresets,} from 'rete-auto-arrange-plugin';

import {MinimapExtra, MinimapPlugin} from 'rete-minimap-plugin';
import {RerouteExtensions, RerouteExtra, ReroutePlugin,} from 'rete-connection-reroute-plugin';
import {isValueNode, PrimitiveNode, Schemes, vueModel} from "@/components/model";
import ValueNodeView from "@/components/floweditor/ValueNodeView.vue";
import ValueConnectionView from "@/components/floweditor/ValueConnectionView.vue";
import {Position} from "rete-area-plugin/_types/types";
import {setupBlockSelection} from "@/components/floweditor/selection/editor";

type AreaExtra =
    | Area2D<Schemes>
    | VueArea2D<Schemes>
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
    // The menu key (or Shift+F10) was pressed (06 §4.4).
    onMenuKey: (() => void) | undefined;
    // Where "Arrange blocks" would put each block, by the editor's node id (06 §4.4): computed
    // by the auto-arrange plugin, applied by the engine.
    arrangement: (() => Promise<Map<string, Position>>) | undefined;

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
    // The arrangement is computed only: each block is moved by its changenode's event.
    editor.arrangement = async () => {
        const nodes = editor.getNodes().filter(n => n instanceof PrimitiveNode);
        const connections = editor.getConnections().filter(c => !isValueNode(editor.getNode(c.source)));
        const keep = {setEditor() {}, setArea() {}, async apply() {}};
        const {result} = await arrange.layout({nodes, connections, applier: keep as never});
        const positions = new Map<string, Position>();
        for (const child of result.children ?? [])
            positions.set(child.id, {x: child.x ?? 0, y: child.y ?? 0});
        return positions;
    };
    area.use(vueRender);
    area.use(connection);
    area.use(minimap);

    // Only the main button's presses reach the reroute plugin (which listens through the Vue
    // renderer): a right-click on a connection opens its menu (06 §4.4) instead of inserting a
    // pin under the pointer.
    vueRender.addPipe(context => {
        if (context.type === 'pointerdown' && (context.data as { event: PointerEvent }).event.button !== 0)
            return undefined;
        return context;
    });
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
