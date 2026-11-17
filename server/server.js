// Collab Free - servidor de salas para el mod de Geometry Dash
// Sin dependencias: solo Node.js (v18 o más nuevo).
// Funciona con "long polling": cada jugador deja una petición abierta y el
// servidor le responde en cuanto hay algo nuevo (cambios, cursores, invitaciones).

const http = require("http");
const crypto = require("crypto");

const PORT = process.env.PORT || 3000;
const MAX_MEMBERS = 6;
const POLL_TIMEOUT_MS = 25000;      // cuánto espera una petición de /poll
const CLIENT_TIMEOUT_MS = 90000;    // si un jugador no pregunta en 90 s, se le saca
const ROOM_EMPTY_TTL_MS = 10 * 60000;      // sala vacía y sin nadie por volver: 10 min
const ROOM_RETURN_TTL_MS = 24 * 3600000;   // sala con gente "por volver": hasta 24 h (mientras el servidor esté prendido)
const MAX_BODY = 60 * 1024 * 1024;  // 60 MB (niveles grandes)
const MAX_QUEUE = 20000;            // eventos pendientes por jugador

// ---------------------------------------------------------------- estado

/** clientId -> cliente */
const clients = new Map();
/** roomId -> sala */
const rooms = new Map();

// registro de lo que pasa (para revisar problemas en /debug)
const logLines = [];
function log(msg) {
    const line = new Date().toISOString().slice(11, 19) + " " + msg;
    console.log(line);
    logLines.push(line);
    if (logLines.length > 200) logLines.shift();
}

function newId(bytes = 6) {
    return crypto.randomBytes(bytes).toString("hex");
}

function now() {
    return Date.now();
}

// ---------------------------------------------------------------- clientes

function makeClient(accountID, username) {
    const client = {
        id: newId(),
        token: newId(16),
        accountID,
        username,
        lastSeen: now(),
        queue: [],      // eventos todavía no entregados
        seq: 0,         // número del último evento creado
        waiter: null,   // petición /poll esperando
        roomId: null,
        state: "edit",  // edit = editando, test = probando, away = fuera del editor
    };
    clients.set(client.id, client);
    return client;
}

function auth(body) {
    const client = clients.get(String(body.client || ""));
    if (!client || client.token !== body.token) return null;
    client.lastSeen = now();
    return client;
}

function pushEvent(client, event) {
    // cursores y cubos se fusionan: solo importa la última posición
    if (event.type === "cursor" || event.type === "player") {
        for (let i = client.queue.length - 1; i >= 0; i--) {
            const e = client.queue[i];
            if (e.type === event.type && e.from === event.from) {
                const seq = e.seq;
                Object.assign(e, event);
                e.seq = seq;
                flush(client);
                return;
            }
        }
    }
    client.seq += 1;
    event.seq = client.seq;
    client.queue.push(event);
    if (client.queue.length > MAX_QUEUE) {
        // demasiado atrasado: se le pide que se vuelva a sincronizar
        client.queue = [];
        client.seq += 1;
        client.queue.push({ type: "resync", seq: client.seq });
    }
    flush(client);
}

function flush(client) {
    if (!client.waiter || client.queue.length === 0) return;
    const { res, timer } = client.waiter;
    client.waiter = null;
    clearTimeout(timer);
    sendJson(res, 200, { events: client.queue });
}

function dropAcked(client, since) {
    if (typeof since !== "number") return;
    client.queue = client.queue.filter((e) => e.seq > since);
}

// explicit = salió con el botón; si no, se desconectó y vuelve al abrir el juego
function removeClient(client, explicit = false) {
    const room = rooms.get(client.roomId);
    if (room && !explicit) room.returning.add(client.accountID);
    leaveRoom(client, !explicit);
    if (client.waiter) {
        clearTimeout(client.waiter.timer);
        sendJson(client.waiter.res, 200, { events: [] });
        client.waiter = null;
    }
    clients.delete(client.id);
}

// ---------------------------------------------------------------- salas

function publicMember(c) {
    return { client: c.id, accountID: c.accountID, username: c.username, state: c.state || "edit" };
}

function roomMembers(room) {
    return [...room.members].map((id) => clients.get(id)).filter(Boolean);
}

function broadcast(room, event, exceptId = null) {
    for (const member of roomMembers(room)) {
        if (member.id !== exceptId) pushEvent(member, { ...event });
    }
}

function leaveRoom(client, disconnected = false) {
    const room = rooms.get(client.roomId);
    client.roomId = null;
    if (!room) return;
    log(`${client.username} salió de la sala ${room.id}`);
    room.members.delete(client.id);
    room.cursors.delete(client.id);
    room.players.delete(client.id);
    if (room.owner === client.id) {
        // el anfitrión se fue: el siguiente pasa a ser anfitrión
        const next = [...room.members][0];
        room.owner = next || null;
    }
    broadcast(room, {
        type: "leave",
        member: publicMember(client),
        owner: room.owner,
        disconnected,
    });
    if (room.members.size === 0) room.emptySince = now();
}

function roomSnapshot(room) {
    return {
        roomId: room.id,
        levelName: room.levelName,
        header: room.header,
        meta: room.meta,
        objects: [...room.objects.entries()],
        owner: room.owner,
        members: roomMembers(room).map(publicMember),
    };
}

// ---------------------------------------------------------------- rutas

const routes = {
    // primer saludo: el mod se registra con su cuenta de GD
    "/hello": (body) => {
        const accountID = Number(body.accountID) || 0;
        const username = String(body.username || "").slice(0, 32);
        if (accountID <= 0 || !username) {
            return [400, { error: "Necesitas iniciar sesión en tu cuenta de Geometry Dash." }];
        }
        // si ya había una conexión vieja de esta cuenta, se reemplaza
        for (const c of [...clients.values()]) {
            if (c.accountID === accountID) removeClient(c);
        }
        const client = makeClient(accountID, username);
        log(`hola: ${username} (${accountID})`);
        // ¿seguía en una sala? vuelve a entrar ("fuera del editor")
        let roomInfo = null;
        for (const room of rooms.values()) {
            if (!room.returning.has(accountID)) continue;
            room.returning.delete(accountID);
            if (room.members.size >= MAX_MEMBERS) continue;
            client.state = "away";
            room.members.add(client.id);
            room.emptySince = null;
            if (!room.owner) room.owner = client.id;
            client.roomId = room.id;
            broadcast(room, { type: "join", member: publicMember(client) }, client.id);
            log(`${username} volvió a la sala ${room.id}`);
            roomInfo = {
                roomId: room.id,
                levelName: room.levelName,
                owner: room.owner,
                members: roomMembers(room).map(publicMember),
            };
            break;
        }
        return [200, { client: client.id, token: client.token, room: roomInfo }];
    },

    // ¿cuáles de estas cuentas tienen el mod abierto ahora?
    "/online": (body, client) => {
        const wanted = new Set((body.accountIDs || []).map(Number));
        const online = [];
        for (const c of clients.values()) {
            if (c !== client && wanted.has(c.accountID)) online.push(c.accountID);
        }
        return [200, { online }];
    },

    "/room/create": (body, client) => {
        if (client.roomId && rooms.has(client.roomId)) {
            return [400, { error: "Ya estás en una sala. Sal de ella primero." }];
        }
        const room = {
            id: newId(4),
            levelName: String(body.levelName || "Nivel").slice(0, 40),
            header: String(body.header || ""),
            objects: new Map(),
            owner: client.id,
            members: new Set([client.id]),
            cursors: new Map(),
            players: new Map(),
            meta: String(body.meta || ""),
            allowed: new Set([client.accountID]), // cuentas que pueden entrar
            returning: new Set(), // cuentas que se desconectaron sin salir
            emptySince: null,
        };
        for (const [id, str] of body.objects || []) {
            room.objects.set(String(id), String(str));
        }
        rooms.set(room.id, room);
        client.roomId = room.id;
        log(`sala ${room.id} creada por ${client.username} (${room.objects.size} objetos)`);
        return [200, { roomId: room.id }];
    },

    "/room/invite": (body, client) => {
        const room = rooms.get(client.roomId);
        if (!room) return [400, { error: "No estás en una sala." }];
        const targetAccount = Number(body.accountID) || 0;
        const targetName = String(body.username || "").toLowerCase();
        let target = null;
        for (const c of clients.values()) {
            if (targetAccount && c.accountID === targetAccount) target = c;
            else if (!targetAccount && targetName && c.username.toLowerCase() === targetName) target = c;
        }
        if (!target) {
            log(`invitación fallida de ${client.username}: ${targetAccount || targetName} no conectado`);
            return [404, { error: "Ese jugador no tiene el mod abierto ahora." }];
        }
        if (target.id === client.id) return [400, { error: "No te puedes invitar a ti mismo." }];
        if (room.members.has(target.id)) return [400, { error: "Ya está en la sala." }];
        if (room.members.size >= MAX_MEMBERS) return [400, { error: "La sala está llena (máximo 6)." }];
        room.allowed.add(target.accountID);
        const busy = !!(target.roomId && target.roomId !== room.id && rooms.has(target.roomId));
        log(`${client.username} invitó a ${target.username} a la sala ${room.id}${busy ? " (está en otra sala)" : ""}`);
        pushEvent(target, {
            type: "invite",
            roomId: room.id,
            levelName: room.levelName,
            from: publicMember(client),
        });
        return [200, { ok: true, username: target.username, busy }];
    },

    "/room/join": (body, client) => {
        const room = rooms.get(String(body.roomId || ""));
        if (!room) return [404, { error: "Esa sala ya no existe." }];
        if (!room.allowed.has(client.accountID)) {
            return [403, { error: "No tienes invitación a esa sala." }];
        }
        if (!room.members.has(client.id) && room.members.size >= MAX_MEMBERS) {
            return [400, { error: "La sala está llena (máximo 6)." }];
        }
        if (client.roomId && client.roomId !== room.id && rooms.has(client.roomId)) {
            return [400, { error: "Ya estás en otra sala. Sal de ella primero." }];
        }
        room.returning.delete(client.accountID);
        room.members.add(client.id);
        room.emptySince = null;
        if (body.state) client.state = String(body.state);
        if (!room.owner) room.owner = client.id;
        client.roomId = room.id;
        broadcast(room, { type: "join", member: publicMember(client) }, client.id);
        log(`${client.username} entró a la sala ${room.id} (${room.members.size}/6)`);
        // se envían también los cursores actuales
        const snap = roomSnapshot(room);
        snap.cursors = [...room.cursors.entries()].map(([id, p]) => ({ from: id, x: p.x, y: p.y }));
        snap.players = [...room.players.values()];
        return [200, snap];
    },

    "/room/leave": (body, client) => {
        const room = rooms.get(client.roomId);
        if (room) room.returning.delete(client.accountID);
        leaveRoom(client);
        return [200, { ok: true }];
    },

    // cambios de objetos: a = agregar, m = modificar, d = borrar
    "/room/ops": (body, client) => {
        const room = rooms.get(client.roomId);
        if (!room) return [400, { error: "No estás en una sala." }];
        const ops = [];
        for (const op of body.ops || []) {
            const id = String(op.id || "");
            if (!id) continue;
            if (op.t === "a" || op.t === "m") {
                room.objects.set(id, String(op.s || ""));
                ops.push({ t: op.t, id, s: String(op.s || "") });
            } else if (op.t === "d") {
                room.objects.delete(id);
                ops.push({ t: "d", id });
            } else if (op.t === "h") {
                // ajustes del nivel (colores, canción...)
                room.header = String(op.s || "");
                ops.push({ t: "h", id, s: room.header });
            } else if (op.t === "g") {
                // canción, efectos de sonido y pista del nivel
                room.meta = String(op.s || "");
                ops.push({ t: "g", id, s: room.meta });
            }
        }
        if (ops.length) {
            // también se le devuelve al que lo envió, para que sepa el orden
            broadcast(room, { type: "ops", from: client.id, ops });
        }
        return [200, { ok: true }];
    },

    "/room/cursor": (body, client) => {
        const room = rooms.get(client.roomId);
        if (!room) return [400, { error: "No estás en una sala." }];
        const x = Number(body.x) || 0;
        const y = Number(body.y) || 0;
        room.cursors.set(client.id, { x, y });
        broadcast(room, { type: "cursor", from: client.id, x, y }, client.id);
        return [200, { ok: true }];
    },

    // dónde está cada uno: editando, probando el nivel o fuera del editor
    "/room/status": (body, client) => {
        const state = ["edit", "test", "away"].includes(body.state) ? body.state : "edit";
        client.state = state;
        const room = rooms.get(client.roomId);
        if (!room) return [400, { error: "No estás en una sala." }];
        broadcast(room, { type: "status", from: client.id, state }, client.id);
        return [200, { ok: true }];
    },

    // el cubo de quien está probando el nivel
    "/room/player": (body, client) => {
        const room = rooms.get(client.roomId);
        if (!room) return [400, { error: "No estás en una sala." }];
        const ev = {
            type: "player",
            from: client.id,
            on: !!body.on,
            x: Number(body.x) || 0,
            y: Number(body.y) || 0,
            r: Number(body.r) || 0,
            m: Number(body.m) || 0,
            i: Number(body.i) || 1,
            s: Number(body.s) || 1,
            f: !!body.f,
            c1: Number(body.c1) || 0,
            c2: Number(body.c2) || 0,
        };
        if (ev.on) room.players.set(client.id, ev);
        else room.players.delete(client.id);
        broadcast(room, ev, client.id);
        return [200, { ok: true }];
    },

    "/room/info": (body, client) => {
        const room = rooms.get(client.roomId);
        if (!room) return [200, { room: null }];
        return [200, { room: { roomId: room.id, owner: room.owner, members: roomMembers(room).map(publicMember) } }];
    },
};

// ---------------------------------------------------------------- http

function sendJson(res, status, data) {
    if (res.writableEnded) return;
    const text = JSON.stringify(data);
    res.writeHead(status, {
        "Content-Type": "application/json; charset=utf-8",
        "Content-Length": Buffer.byteLength(text),
        "Cache-Control": "no-store",
    });
    res.end(text);
}

function readBody(req) {
    return new Promise((resolve, reject) => {
        const chunks = [];
        let size = 0;
        req.on("data", (chunk) => {
            size += chunk.length;
            if (size > MAX_BODY) {
                reject(new Error("too big"));
                req.destroy();
                return;
            }
            chunks.push(chunk);
        });
        req.on("end", () => resolve(Buffer.concat(chunks).toString("utf8")));
        req.on("error", reject);
    });
}

const server = http.createServer(async (req, res) => {
    const url = new URL(req.url, "http://localhost");

    if (req.method === "GET" && url.pathname === "/debug") {
        const online = [...clients.values()].map((c) => `${c.username} ${c.roomId ? "(sala " + c.roomId + ")" : ""}`);
        const roomList = [...rooms.values()].map((r) => ({
            id: r.id,
            nivel: r.levelName,
            miembros: roomMembers(r).map((c) => `${c.username} (${c.state})`),
            porVolver: r.returning.size,
            objetos: r.objects.size,
        }));
        sendJson(res, 200, { conectados: online, salas: roomList, registro: logLines.slice(-80) });
        return;
    }

    if (req.method === "GET" && (url.pathname === "/" || url.pathname === "/health")) {
        sendJson(res, 200, {
            ok: true,
            name: "Collab Free",
            clients: clients.size,
            rooms: rooms.size,
        });
        return;
    }

    if (req.method !== "POST") {
        sendJson(res, 404, { error: "No encontrado" });
        return;
    }

    let body;
    try {
        const text = await readBody(req);
        body = text ? JSON.parse(text) : {};
    } catch (e) {
        sendJson(res, 400, { error: "Petición inválida" });
        return;
    }

    // /poll se maneja aparte porque se queda esperando
    if (url.pathname === "/poll") {
        const client = auth(body);
        if (!client) {
            sendJson(res, 401, { error: "relogin" });
            return;
        }
        dropAcked(client, Number(body.since));
        if (client.waiter) {
            clearTimeout(client.waiter.timer);
            sendJson(client.waiter.res, 200, { events: [] });
            client.waiter = null;
        }
        if (client.queue.length > 0) {
            sendJson(res, 200, { events: client.queue });
            return;
        }
        const timer = setTimeout(() => {
            if (client.waiter && client.waiter.res === res) client.waiter = null;
            sendJson(res, 200, { events: [] });
        }, POLL_TIMEOUT_MS);
        client.waiter = { res, timer };
        req.on("close", () => {
            if (client.waiter && client.waiter.res === res) {
                clearTimeout(timer);
                client.waiter = null;
            }
        });
        return;
    }

    const handler = routes[url.pathname];
    if (!handler) {
        sendJson(res, 404, { error: "No encontrado" });
        return;
    }

    let client = null;
    if (url.pathname !== "/hello") {
        client = auth(body);
        if (!client) {
            sendJson(res, 401, { error: "relogin" });
            return;
        }
    }

    try {
        const [status, data] = handler(body, client);
        sendJson(res, status, data);
    } catch (e) {
        console.error(e);
        sendJson(res, 500, { error: "Error del servidor" });
    }
});

// limpieza periódica
setInterval(() => {
    const t = now();
    for (const client of [...clients.values()]) {
        if (!client.waiter && t - client.lastSeen > CLIENT_TIMEOUT_MS) {
            removeClient(client);
        }
    }
    for (const [id, room] of rooms) {
        if (room.members.size > 0 || !room.emptySince) continue;
        const ttl = room.returning.size > 0 ? ROOM_RETURN_TTL_MS : ROOM_EMPTY_TTL_MS;
        if (t - room.emptySince > ttl) {
            log(`sala ${id} cerrada (vacía)`);
            rooms.delete(id);
        }
    }
}, 10000);

server.keepAliveTimeout = 65000;
server.headersTimeout = 66000;
server.requestTimeout = 0;

server.listen(PORT, () => {
    console.log(`Collab Free escuchando en el puerto ${PORT}`);
});
