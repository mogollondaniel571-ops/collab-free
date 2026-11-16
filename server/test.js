// Prueba rápida: 3 jugadores, invitaciones, cambios, cursores y cambio de anfitrión.
// Uso: node server.js  (en otra terminal)  y luego  node test.js
const BASE = process.env.URL || "http://localhost:3000";

async function post(path, body) {
    const r = await fetch(BASE + path, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
    });
    return [r.status, await r.json()];
}

function assert(cond, msg) {
    if (!cond) { console.error("FALLÓ:", msg); process.exit(1); }
    console.log("ok -", msg);
}

async function hello(id, name) {
    const [s, d] = await post("/hello", { accountID: id, username: name });
    assert(s === 200, `hello ${name}`);
    return { ...d, since: 0, accountID: id, name };
}

async function poll(c) {
    const [s, d] = await post("/poll", { client: c.client, token: c.token, since: c.since });
    assert(s === 200, `poll ${c.name}`);
    if (d.events.length) c.since = d.events[d.events.length - 1].seq;
    return d.events;
}

(async () => {
    const a = await hello(1, "Dani");
    const b = await hello(2, "Amigo1");
    const c = await hello(3, "Amigo2");
    const auth = (x) => ({ client: x.client, token: x.token });

    let [s, d] = await post("/online", { ...auth(a), accountIDs: [2, 3, 99] });
    assert(s === 200 && d.online.length === 2, "online muestra 2 amigos");

    [s, d] = await post("/room/create", { ...auth(a), levelName: "Mi nivel", header: "kS38,xx", objects: [["a:1", "1,1,2,15,3,15"]] });
    assert(s === 200 && d.roomId, "crear sala");
    const roomId = d.roomId;

    [s, d] = await post("/room/join", { ...auth(c), roomId });
    assert(s === 403, "sin invitación no se puede entrar");

    [s, d] = await post("/room/invite", { ...auth(a), accountID: 2 });
    assert(s === 200, "invitar por cuenta");
    [s, d] = await post("/room/invite", { ...auth(a), username: "amigo2" });
    assert(s === 200, "invitar por nombre");

    let ev = await poll(b);
    assert(ev.some((e) => e.type === "invite" && e.roomId === roomId), "le llega la invitación");

    [s, d] = await post("/room/join", { ...auth(b), roomId });
    assert(s === 200 && d.objects.length === 1 && d.header === "kS38,xx", "entrar recibe el nivel");
    [s, d] = await post("/room/join", { ...auth(c), roomId });
    assert(s === 200 && d.members.length === 3, "3 en la sala");

    ev = await poll(a);
    assert(ev.filter((e) => e.type === "join").length === 2, "anfitrión ve que entraron 2");

    await post("/room/ops", { ...auth(b), ops: [{ t: "a", id: "b:1", s: "1,8,2,45,3,15" }, { t: "m", id: "a:1", s: "1,1,2,30,3,15" }] });
    ev = await poll(a);
    const ops = ev.find((e) => e.type === "ops");
    assert(ops && ops.ops.length === 2 && ops.from === b.client, "anfitrión recibe los cambios");
    ev = await poll(b);
    assert(ev.some((e) => e.type === "ops" && e.from === b.client), "el que cambió recibe su eco");

    await post("/room/cursor", { ...auth(c), x: 10, y: 20 });
    await post("/room/cursor", { ...auth(c), x: 30, y: 40 });
    ev = await poll(a);
    const cur = ev.filter((e) => e.type === "cursor");
    assert(cur.length === 1 && cur[0].x === 30, "cursores se fusionan");

    await post("/room/leave", auth(a));
    ev = await poll(b);
    const lv = ev.find((e) => e.type === "leave");
    assert(lv && lv.owner === b.client, "si el anfitrión se va, otro toma su lugar");

    await post("/room/ops", { ...auth(c), ops: [{ t: "d", id: "a:1" }] });
    const d2 = await hello(4, "Nuevo");
    await post("/room/invite", { ...auth(b), accountID: 4 });
    [s, d] = await post("/room/join", { ...auth(d2), roomId });
    assert(s === 200 && d.objects.length === 1 && d.objects[0][0] === "b:1", "el que entra tarde recibe el estado actual");

    // long poll espera y se despierta
    const t0 = Date.now();
    const p = poll(a);
    setTimeout(() => post("/room/invite", { ...auth(b), accountID: 1 }), 300);
    ev = await p;
    assert(ev.some((e) => e.type === "invite") && Date.now() - t0 < 5000, "long poll se despierta al instante");

    console.log("\nTodo bien ✔");
    process.exit(0);
})();
