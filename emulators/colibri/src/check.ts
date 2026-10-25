// Checking JSON data read from files and clients: each problem is recorded with the path of the
// value it concerns (`channels[1].min`), and reading goes on, so one pass reports every problem.

export type Json = null | boolean | number | string | Json[] | {[key: string]: Json};
export type JsonObject = {[key: string]: Json};

// The longest metadata string hakkas takes (04 §0, "Validation").
export const STRING_MAX = 1024;

export class Checker {
    readonly errors: string[] = [];
    readonly warnings: string[] = [];

    error(path: string, message: string): undefined {
        this.errors.push(path === '' ? message : `${path}: ${message}`);
        return undefined;
    }

    warn(path: string, message: string): void {
        this.warnings.push(path === '' ? message : `${path}: ${message}`);
    }

    get ok(): boolean {
        return this.errors.length === 0;
    }

    object(v: Json | undefined, path: string): JsonObject | undefined {
        if (isObject(v))
            return v;
        return this.error(path, 'expected an object');
    }

    array(v: Json | undefined, path: string): Json[] | undefined {
        if (Array.isArray(v))
            return v;
        return this.error(path, 'expected an array');
    }

    // Warns about members not in `known` (most likely a typing error in a hand-written file).
    members(o: JsonObject, known: readonly string[], path: string): void {
        for (const key of Object.keys(o))
            if (!known.includes(key))
                this.warn(join(path, key), 'unknown member, ignored');
    }

    string(o: JsonObject, key: string, path: string, required = false): string | undefined {
        const v = o[key];
        if (v === undefined)
            return required ? this.error(join(path, key), 'missing') : undefined;
        if (typeof v !== 'string')
            return this.error(join(path, key), 'expected a string');
        if (Buffer.byteLength(v) > STRING_MAX)
            return this.error(join(path, key), `longer than ${STRING_MAX} bytes`);
        return v;
    }

    number(o: JsonObject, key: string, path: string, required = false): number | undefined {
        const v = o[key];
        if (v === undefined)
            return required ? this.error(join(path, key), 'missing') : undefined;
        if (typeof v !== 'number' || !Number.isFinite(v))
            return this.error(join(path, key), 'expected a number');
        return v;
    }

    integer(o: JsonObject, key: string, path: string, min: number, max: number,
            required = false): number | undefined {
        const v = o[key];
        if (v === undefined)
            return required ? this.error(join(path, key), 'missing') : undefined;
        if (typeof v !== 'number' || !Number.isInteger(v) || v < min || v > max)
            return this.error(join(path, key), `expected an integer from ${min} to ${max}`);
        return v;
    }

    boolean(o: JsonObject, key: string, path: string, required = false): boolean | undefined {
        const v = o[key];
        if (v === undefined)
            return required ? this.error(join(path, key), 'missing') : undefined;
        if (typeof v !== 'boolean')
            return this.error(join(path, key), 'expected true or false');
        return v;
    }

    oneOf<T extends string>(o: JsonObject, key: string, path: string, values: readonly T[],
                            required = false): T | undefined {
        const v = o[key];
        if (v === undefined)
            return required ? this.error(join(path, key), 'missing') : undefined;
        if (typeof v !== 'string' || !(values as readonly string[]).includes(v))
            return this.error(join(path, key), `expected one of ${values.join(', ')}`);
        return v as T;
    }
}

export function isObject(v: Json | undefined): v is JsonObject {
    return typeof v === 'object' && v !== null && !Array.isArray(v);
}

export function join(path: string, key: string | number): string {
    if (typeof key === 'number')
        return `${path}[${key}]`;
    return path === '' ? key : `${path}.${key}`;
}
