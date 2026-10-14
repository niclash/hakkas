// The process list's logic (06 §4.6). Kept free of Vue so that it can be unit tested.

// How long a process has run, for people: "less than a minute", "5 min", "2 h 5 min", "3 d 4 h".
export function formatUptime(seconds: number): string {
    const minutes = Math.floor(seconds / 60);
    if (minutes < 1)
        return "less than a minute";
    if (minutes < 60)
        return `${minutes} min`;
    const hours = Math.floor(minutes / 60);
    if (hours < 24)
        return minutes % 60 === 0 ? `${hours} h` : `${hours} h ${minutes % 60} min`;
    const days = Math.floor(hours / 24);
    return hours % 24 === 0 ? `${days} d` : `${days} d ${hours % 24} h`;
}

// The uptime now: the status's uptime (seconds, at `at` ms) plus the time since, while it runs.
export function uptimeNow(uptime: number, at: number, running: boolean, now: number): number {
    return running ? uptime + Math.max(0, Math.floor((now - at) / 1000)) : uptime;
}

// Whether a network:error is the failure of saving the process `id` (02 §3.3 persist).
export function isSaveFailure(message: string, id: string): boolean {
    return message.startsWith("Unable to save graph: ") && message.endsWith(` (${id})`);
}

// A random UUID v4, lowercase, as a new process's id (02 §3.2 new_graph). crypto.randomUUID()
// needs a secure context, and a device's page is often plain http on its address.
export function newProcessId(random: (bytes: Uint8Array) => Uint8Array = b => crypto.getRandomValues(b)): string {
    const b = random(new Uint8Array(16));
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    const hex = [...b].map(x => x.toString(16).padStart(2, "0")).join("");
    return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}
