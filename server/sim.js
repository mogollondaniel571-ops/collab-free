// Simulación: 3 jugadores editando los mismos objetos al mismo tiempo,
// con la misma lógica que usa el mod (cola de envío + "pendientes").
// Al final todos deben tener exactamente el mismo nivel que el servidor.
const BASE = process.env.URL || "http://localhost:3000";

async function post(path, body) {
    const r = await fetch(BASE + path, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
    return [r.status, await r.json()];
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const rand = (n) => Math.floor(Math.random() * n);

class Client {
    constructor(name, acc) { this.name = name; this.acc = acc; this.objs = new Map(); this.pending = new Map(); this.out = []; this.sending = false; this.since = 0; this.counter = 0; this.stop = false; }
    auth() { return { client: this.client, token: this.token }; }
    async hello() { const [, d] = await post("/hello", { accountID: this.acc, username: this.name }); this.client = d.client; this.token = d.token; }
    queue(t, id, s) { this.out.push(t === "d" ? { t, id } : { t, id, s }); this.pending.set(id, (this.pending.get(id) || 0) + 1); }
    localEdit() {
        const ids = [...this.objs.keys()];
        const r = Math.random();
        if (r < 0.3 || ids.length === 0) { const id = `${this.name}-${++this.counter}`; const s = `obj:${this.name}:${rand(1000)}`; this.objs.set(id, s); this.queue("a", id, s); }
        else if (r < 0.85) { const id = ids[rand(ids.length)]; const s = `mod:${this.name}:${rand(1000)}`; this.objs.set(id, s); this.queue("m", id, s); }
        else { const id = ids[rand(ids.length)]; this.objs.delete(id); this.queue("d", id); }
    }
    async flush() {
        if (this.sending || this.out.length === 0) return;
        this.sending = true;
        const batch = this.out; this.out = [];
        await sleep(rand(40)); // red lenta
        await post("/room/ops", { ...this.auth(), ops: batch });
        this.sending = false;
    }
    apply(ev) {
        if (ev.type !== "ops") return;
        if (ev.from === this.client) {
            for (const op of ev.ops) { const n = (this.pending.get(op.id) || 0) - 1; if (n <= 0) this.pending.delete(op.id); else this.pending.set(op.id, n); }
            return;
        }
        for (const op of ev.ops) {
            if (this.pending.has(op.id)) continue;
            if (op.t === "d") this.objs.delete(op.id); else this.objs.set(op.id, op.s);
        }
    }
    async pollLoop() {
        while (!this.stop) {
            const [, d] = await post("/poll", { ...this.auth(), since: this.since });
            for (const e of d.events) this.since = Math.max(this.since, e.seq);
            await sleep(rand(30));
            for (const e of d.events) this.apply(e);
        }
    }
}

(async () => {
    const cs = [new Client("A", 11), new Client("B", 12), new Client("C", 13)];
    for (const c of cs) await c.hello();
    const [, room] = await post("/room/create", { ...cs[0].auth(), levelName: "sim", header: "h", objects: [] });
    for (const c of cs.slice(1)) {
        await post("/room/invite", { ...cs[0].auth(), accountID: c.acc });
        const [, snap] = await post("/room/join", { ...c.auth(), roomId: room.roomId });
        for (const [id, s] of snap.objects) c.objs.set(id, s);
    }
    // vaciar eventos de invitación/entrada
    for (const c of cs) c.pollLoop();

    // 4 segundos de edición caótica
    const end = Date.now() + 4000;
    while (Date.now() < end) {
        for (const c of cs) { if (Math.random() < 0.7) c.localEdit(); c.flush(); }
        await sleep(15);
    }
    // dejar que todo llegue
    for (let i = 0; i < 40; i++) { for (const c of cs) c.flush(); await sleep(50); }
    await sleep(500);

    const [, snap] = await post("/room/join", { ...cs[0].auth(), roomId: room.roomId });
    const server = new Map(snap.objects);
    let ok = true;
    for (const c of cs) {
        const same = c.objs.size === server.size && [...server].every(([k, v]) => c.objs.get(k) === v);
        console.log(`${c.name}: ${c.objs.size} objetos, pendientes=${c.pending.size} -> ${same ? "IGUAL al servidor ✔" : "DISTINTO ✘"}`);
        ok = ok && same;
    }
    console.log(`servidor: ${server.size} objetos`);
    process.exit(ok ? 0 : 1);
})();
