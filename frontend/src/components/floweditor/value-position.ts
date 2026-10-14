// Where a value node is drawn (06 §4.3). Kept free of Rete so that it can be unit tested.

export type Position = { x: number, y: number };

// The space between a value node and its block.
export const VALUE_GAP = 40;

// Left of the block, its middle level with the input's socket (`socketY` below the block's top).
export function valuePosition(block: Position, socketY: number, value: { width: number, height: number }): Position {
    return {x: block.x - value.width - VALUE_GAP, y: block.y + socketY - value.height / 2};
}
