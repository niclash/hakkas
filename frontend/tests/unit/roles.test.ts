import {describe, expect, it} from "vitest";
import {allowed, permissions, roleLabel} from "@/components/roles";
import {menuFor} from "@/components/floweditor/menus";

// 06 §4.11: what each role is offered, from its login's capabilities (02 §2.4).

const viewer = ["protocol:component", "network:status", "network:data", "graph:readonly"];
const operator = [...viewer, "network:control"];
const editor = [...operator, "protocol:graph", "protocol:runtime", "network:persist"];

describe("permissions", () => {
    it("per role", () => {
        expect(permissions(viewer)).toEqual({edit: false, control: false, persist: false, manage: false, watch: true});
        expect(permissions(operator)).toEqual({edit: false, control: true, persist: false, manage: false, watch: true});
        expect(permissions(editor)).toEqual({edit: true, control: true, persist: true, manage: true, watch: true});
        expect(permissions([]).watch).toBe(false);
    });

    it("the top bar's label", () => {
        expect(roleLabel(permissions(viewer))).toBe("Viewer — read only");
        expect(roleLabel(permissions(operator))).toBe("Operator — starts and stops, no editing");
        expect(roleLabel(permissions(editor))).toBe("");
    });

    it("a viewer's menus keep only what does not change anything", () => {
        const p = permissions(viewer);
        const offered = (t: Parameters<typeof menuFor>[0]) => menuFor(t).map(i => i.action).filter(a => allowed(a, p));
        expect(offered({kind: "canvas", canPaste: true})).toEqual(["select-all", "zoom-fit", "watch-all"]);
        expect(offered({kind: "block", id: "a", selected: 1})).toEqual(["watch-inputs", "copy"]);
        expect(offered({kind: "connection"})).toEqual(["watch"]);
        expect(offered({kind: "port", direction: "in", name: "x"})).toEqual([]);
        // An editor is offered everything.
        const all = permissions(editor);
        expect(menuFor({kind: "block", id: "a", selected: 1}).every(i => allowed(i.action, all))).toBe(true);
    });
});
