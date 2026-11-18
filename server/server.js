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
const KEY_GRACE_MS = 10 * 60000;    // una cuenta con otra llave solo entra si la anterior lleva 10 min sin conectarse
const KEEP_AWAKE_MS = 10 * 60000;   // se visita a sí mismo para que Render no lo duerma

// ---------------------------------------------------------------- estado

/** clientId -> cliente */
const clients = new Map();
/** roomId -> sala */
const rooms = new Map();
/** accountID -> { hash de la llave, última vez que se usó } (protege tu cuenta) */
const accountKeys = new Map();
/** accountID -> Map(roomId -> invitación) (se guardan aunque cierres el juego) */
const pendingInvites = new Map();

function hashKey(key) {
    return crypto.createHash("sha256").update(String(key)).digest("hex");
}

function addInvite(accountID, invite) {
    if (!pendingInvites.has(accountID)) pendingInvites.set(accountID, new Map());
    pendingInvites.get(accountID).set(invite.roomId, invite);
}

function removeInvite(accountID, roomId) {
    const map = pendingInvites.get(accountID);
    if (map) map.delete(roomId);
}

function invitesFor(accountID) {
    const map = pendingInvites.get(accountID);
    if (!map) return [];
    const list = [];
    for (const [roomId, inv] of map) {
        if (rooms.has(roomId)) list.push(inv);
        else map.delete(roomId);
    }
    return list;
}

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

function publicMember(c, room = null) {
    const r = room || rooms.get(c.roomId);
    return {
        client: c.id,
        accountID: c.accountID,
        username: c.username,
        state: c.state || "edit",
        viewer: !!(r && r.viewers && r.viewers.has(c.accountID)),
    };
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
    if (room.selects.has(client.id)) {
        room.selects.delete(client.id);
        broadcast(room, { type: "select", from: client.id, ids: [] });
    }
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
        members: roomMembers(room).map((c) => publicMember(c, room)),
        allowed: [...room.allowed],
        chat: room.chat,
        selects: [...room.selects.entries()].map(([from, ids]) => ({ from, ids })),
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
        // protección: cada cuenta usa una llave secreta que solo tiene su juego
        const key = String(body.key || "");
        if (key.length >= 16) {
            const hash = hashKey(key);
            const known = accountKeys.get(accountID);
            if (known && known.hash !== hash) {
                const active = [...clients.values()].some((c) => c.accountID === accountID);
                if (active || now() - known.lastUsed < KEY_GRACE_MS) {
                    log(`llave incorrecta para ${username} (${accountID})`);
                    return [403, { error: "Esta cuenta ya se está usando en otro dispositivo. Espera 10 minutos e intenta otra vez." }];
                }
            }
            accountKeys.set(accountID, { hash, lastUsed: now() });
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
                members: roomMembers(room).map((c) => publicMember(c, room)),
            };
            break;
        }
        return [200, { client: client.id, token: client.token, room: roomInfo, invites: invitesFor(accountID) }];
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
            viewers: new Set(),   // cuentas que solo pueden mirar
            chat: [],             // últimos mensajes
            selects: new Map(),   // clientId -> ids seleccionados
            savedAt: 0,           // de cuándo es la copia (al revivir la sala)
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
            if (targetAccount) {
                // no está conectado: la invitación lo espera hasta que abra el juego
                if (room.members.size >= MAX_MEMBERS) return [400, { error: "La sala está llena (máximo 6)." }];
                room.allowed.add(targetAccount);
                addInvite(targetAccount, { roomId: room.id, levelName: room.levelName, from: publicMember(client) });
                log(`${client.username} invitó a ${targetAccount} (no conectado) a la sala ${room.id}`);
                return [200, { ok: true, username: String(body.username || targetAccount), offline: true }];
            }
            log(`invitación fallida de ${client.username}: ${targetName} no conectado`);
            return [404, { error: "Ese jugador no tiene el mod abierto ahora. Invítalo desde tu lista de amigos y le llegará al abrir el juego." }];
        }
        if (target.id === client.id) return [400, { error: "No te puedes invitar a ti mismo." }];
        if (room.members.has(target.id)) return [400, { error: "Ya está en la sala." }];
        if (room.members.size >= MAX_MEMBERS) return [400, { error: "La sala está llena (máximo 6)." }];
        room.allowed.add(target.accountID);
        addInvite(target.accountID, { roomId: room.id, levelName: room.levelName, from: publicMember(client) });
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
        removeInvite(client.accountID, room.id);
        room.members.add(client.id);
        room.emptySince = null;
        if (body.state) client.state = String(body.state);
        if (!room.owner) room.owner = client.id;
        client.roomId = room.id;
        broadcast(room, { type: "join", member: publicMember(client, room) }, client.id);
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
        if (room.viewers.has(client.accountID)) {
            return [403, { error: "Estás en modo solo mirar." }];
        }
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

    // chat dentro del editor
    "/room/chat": (body, client) => {
        const room = rooms.get(client.roomId);
        if (!room) return [400, { error: "No estás en una sala." }];
        const text = String(body.text || "").replace(/[\r\n]+/g, " ").trim().slice(0, 200);
        if (!text) return [400, { error: "Mensaje vacío." }];
        const msg = { from: client.id, username: client.username, text, t: now() };
        room.chat.push(msg);
        if (room.chat.length > 60) room.chat.shift();
        broadcast(room, { type: "chat", ...msg });
        return [200, { ok: true }];
    },

    // "mira aquí": una flecha que ven todos
    "/room/ping": (body, client) => {
        const room = rooms.get(client.roomId);
        if (!room) return [400, { error: "No estás en una sala." }];
        broadcast(room, {
            type: "ping",
            from: client.id,
            username: client.username,
            x: Number(body.x) || 0,
            y: Number(body.y) || 0,
        });
        return [200, { ok: true }];
    },

    // qué objetos tiene seleccionados cada uno
    "/room/select": (body, client) => {
        const room = rooms.get(client.roomId);
        if (!room) return [400, { error: "No estás en una sala." }];
        const ids = (Array.isArray(body.ids) ? body.ids : []).slice(0, 400).map(String);
        if (ids.length) room.selects.set(client.id, ids);
        else room.selects.delete(client.id);
        broadcast(room, { type: "select", from: client.id, ids }, client.id);
        return [200, { ok: true }];
    },

    // ---- controles del anfitrión
    "/room/kick": (body, client) => {
        const room = rooms.get(client.roomId);
        if (!room) return [400, { error: "No estás en una sala." }];
        if (room.owner !== client.id) return [403, { error: "Solo el anfitrión puede hacer eso." }];
        const target = clients.get(String(body.target || ""));
        if (!target || target.roomId !== room.id) return [404, { error: "Esa persona ya no está en la sala." }];
        if (target.id === client.id) return [400, { error: "No te puedes sacar a ti mismo." }];
        room.allowed.delete(target.accountID);
        room.returning.delete(target.accountID);
        room.viewers.delete(target.accountID);
        removeInvite(target.accountID, room.id);
        leaveRoom(target);
        pushEvent(target, { type: "kicked", roomId: room.id, by: client.username });
        log(`${client.username} sacó a ${target.username} de la sala ${room.id}`);
        return [200, { ok: true }];
    },

    "/room/owner": (body, client) => {
        const room = rooms.get(client.roomId);
        if (!room) return [400, { error: "No estás en una sala." }];
        if (room.owner !== client.id) return [403, { error: "Solo el anfitrión puede hacer eso." }];
        const target = clients.get(String(body.target || ""));
        if (!target || target.roomId !== room.id) return [404, { error: "Esa persona ya no está en la sala." }];
        room.owner = target.id;
        room.viewers.delete(target.accountID);
        broadcast(room, { type: "owner", owner: target.id, member: publicMember(target, room) });
        log(`${client.username} le pasó el mando a ${target.username} en la sala ${room.id}`);
        return [200, { ok: true }];
    },

    "/room/role": (body, client) => {
        const room = rooms.get(client.roomId);
        if (!room) return [400, { error: "No estás en una sala." }];
        if (room.owner !== client.id) return [403, { error: "Solo el anfitrión puede hacer eso." }];
        const target = clients.get(String(body.target || ""));
        if (!target || target.roomId !== room.id) return [404, { error: "Esa persona ya no está en la sala." }];
        if (target.id === client.id) return [400, { error: "El anfitrión siempre puede editar." }];
        if (body.viewer) room.viewers.add(target.accountID);
        else room.viewers.delete(target.accountID);
        broadcast(room, { type: "role", client: target.id, viewer: !!body.viewer });
        return [200, { ok: true }];
    },

    // descartar una invitación guardada
    "/invite/dismiss": (body, client) => {
        removeInvite(client.accountID, String(body.roomId || ""));
        return [200, { ok: true }];
    },

    // el servidor se reinició y la sala se perdió: alguien que estaba en ella la vuelve a crear
    // con su copia guardada. Si llega una copia más nueva, reemplaza a la anterior.
    "/room/revive": (body, client) => {
        const roomId = String(body.roomId || "");
        if (!roomId) return [400, { error: "Falta la sala." }];
        const savedAt = Number(body.savedAt) || 0;
        if (client.roomId && client.roomId !== roomId && rooms.has(client.roomId)) {
            return [400, { error: "Ya estás en otra sala." }];
        }
        let room = rooms.get(roomId);
        const allowed = (Array.isArray(body.allowed) ? body.allowed : []).map(Number).filter((n) => n > 0);
        if (room) {
            if (!room.allowed.has(client.accountID)) return [403, { error: "No tienes invitación a esa sala." }];
            if (room.revived && savedAt > room.savedAt) {
                // tu copia es más nueva: gana la tuya
                room.header = String(body.header || room.header);
                room.meta = String(body.meta || room.meta);
                room.objects = new Map();
                for (const [id, str] of body.objects || []) room.objects.set(String(id), String(str));
                room.savedAt = savedAt;
                broadcast(room, { type: "resync" }, client.id);
                log(`sala ${roomId} actualizada con una copia más nueva de ${client.username}`);
            }
        } else {
            if (!allowed.includes(client.accountID)) allowed.push(client.accountID);
            room = {
                id: roomId,
                levelName: String(body.levelName || "Nivel").slice(0, 40),
                header: String(body.header || ""),
                objects: new Map(),
                owner: null,
                members: new Set(),
                cursors: new Map(),
                players: new Map(),
                meta: String(body.meta || ""),
                allowed: new Set(allowed),
                returning: new Set(allowed.filter((a) => a !== client.accountID)),
                viewers: new Set((Array.isArray(body.viewers) ? body.viewers : []).map(Number)),
                chat: [],
                selects: new Map(),
                savedAt,
                revived: true,
                emptySince: null,
            };
            for (const [id, str] of body.objects || []) room.objects.set(String(id), String(str));
            rooms.set(roomId, room);
            log(`sala ${roomId} revivida por ${client.username} (${room.objects.size} objetos)`);
            // los que ya están conectados y eran de la sala vuelven solos
            for (const c of clients.values()) {
                if (c !== client && !c.roomId && room.returning.has(c.accountID) && room.members.size < MAX_MEMBERS - 1) {
                    room.returning.delete(c.accountID);
                    c.state = "away";
                    room.members.add(c.id);
                    c.roomId = room.id;
                    if (!room.owner) room.owner = c.id;
                    pushEvent(c, { type: "revived", roomId: room.id, levelName: room.levelName });
                }
            }
        }
        room.returning.delete(client.accountID);
        room.members.add(client.id);
        room.emptySince = null;
        if (!room.owner) room.owner = client.id;
        if (body.state) client.state = String(body.state);
        client.roomId = room.id;
        broadcast(room, { type: "join", member: publicMember(client, room) }, client.id);
        const snap = roomSnapshot(room);
        snap.cursors = [];
        snap.players = [];
        return [200, snap];
    },

    "/room/info": (body, client) => {
        const room = rooms.get(client.roomId);
        if (!room) return [200, { room: null }];
        return [200, { room: { roomId: room.id, owner: room.owner, members: roomMembers(room).map((c) => publicMember(c, room)) } }];
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

    if (req.method === "GET" && url.pathname === "/ping") {
        sendJson(res, 200, { ok: true });
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

// Render duerme el servidor gratis si nadie lo visita en 15 min (y se pierden las salas).
// Se visita a sí mismo cada 10 min para quedarse despierto.
const SELF_URL = process.env.RENDER_EXTERNAL_URL || process.env.SELF_URL || "";
if (SELF_URL && typeof fetch === "function") {
    setInterval(() => {
        fetch(SELF_URL.replace(/\/$/, "") + "/ping").catch(() => {});
    }, KEEP_AWAKE_MS);
}

server.listen(PORT, () => {
    console.log(`Collab Free escuchando en el puerto ${PORT}`);
});
