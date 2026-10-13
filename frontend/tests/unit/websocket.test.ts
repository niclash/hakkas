import {describe, expect, it} from "vitest";
import {websocketUrl} from "@/components/protocols/websocket";

// The WebSocket is on the page's port + 1 (02 §0).
describe("websocketUrl", () => {
    const page = (protocol: string, hostname: string, port: string) => ({protocol, hostname, port});

    it("uses the page's port + 1", () => {
        expect(websocketUrl(page("http:", "192.168.1.20", "8080"), undefined)).toBe("ws://192.168.1.20:8081/");
    });
    it("uses the scheme's default port when the page has none", () => {
        expect(websocketUrl(page("http:", "hakkas.local", ""), undefined)).toBe("ws://hakkas.local:81/");
        expect(websocketUrl(page("https:", "hakkas.local", ""), undefined)).toBe("wss://hakkas.local:444/");
    });
    it("uses wss for an https page", () => {
        expect(websocketUrl(page("https:", "example.com", "8443"), undefined)).toBe("wss://example.com:8444/");
    });
    it("keeps an IPv6 address in brackets, as browsers report it", () => {
        expect(websocketUrl(page("http:", "[::1]", "8080"), undefined)).toBe("ws://[::1]:8081/");
        expect(websocketUrl(page("http:", "fe80::1", "8080"), undefined)).toBe("ws://[fe80::1]:8081/");
    });
    it("takes VITE_HAKKAS_WS's override as it is", () => {
        expect(websocketUrl(page("http:", "localhost", "5173"), "ws://localhost:8081/")).toBe("ws://localhost:8081/");
    });
});
