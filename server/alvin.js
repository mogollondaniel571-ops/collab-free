// Alvin el wey: bot de Discord sin librerías (solo Node 22+).
// Ruletas, parejas, torneos, ranking, avisos programados y personalidad.
// Se activa solo si existe la variable DISCORD_TOKEN.
"use strict";

const API = process.env.ALVIN_API || "https://discord.com/api/v10";
const ZONA = -5; // Ecuador (UTC-5, sin horario de verano)
const COLOR = 0xf5a623;
const CANAL_MEMORIA = "alvin-memoria";

let TOKEN = "";
let APP_ID = "";
let BOT_ID = "";
const log = (...a) => console.log("[alvin]", ...a);

// ---------------------------------------------------------------- utilidades
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const pick = (a) => a[Math.floor(Math.random() * a.length)];
function shuffle(a) {
  a = a.slice();
  for (let i = a.length - 1; i > 0; i--) {
    const j = Math.floor(Math.random() * (i + 1));
    [a[i], a[j]] = [a[j], a[i]];
  }
  return a;
}
const m = (id) => `<@${id}>`;
function sacarMenciones(txt) {
  const out = [];
  for (const x of String(txt || "").matchAll(/<@!?(\d{5,25})>/g)) if (!out.includes(x[1])) out.push(x[1]);
  return out;
}

// ---------------------------------------------------------------- REST
async function rest(method, path, body, files) {
  for (let intento = 0; intento < 5; intento++) {
    const opts = { method, headers: { Authorization: `Bot ${TOKEN}`, "User-Agent": "AlvinBot (collab-free, 1.0)" } };
    if (files) {
      const fd = new FormData();
      fd.append("payload_json", JSON.stringify(body || {}));
      files.forEach((f, i) => fd.append(`files[${i}]`, new Blob([f.data], { type: "application/json" }), f.name));
      opts.body = fd;
    } else if (body !== undefined) {
      opts.headers["Content-Type"] = "application/json";
      opts.body = JSON.stringify(body);
    }
    let res;
    try {
      res = await fetch(API + path, opts);
    } catch (e) {
      await sleep(1000 * (intento + 1));
      continue;
    }
    if (res.status === 429) {
      let espera = 1;
      try { espera = (await res.json()).retry_after || 1; } catch (_) {}
      await sleep(espera * 1000 + 100);
      continue;
    }
    if (res.status === 204) return null;
    const txt = await res.text();
    let data = null;
    try { data = txt ? JSON.parse(txt) : null; } catch (_) { data = txt; }
    if (!res.ok) {
      const err = new Error(`Discord ${res.status} ${method} ${path}: ${typeof data === "string" ? data : JSON.stringify(data)}`);
      err.status = res.status;
      throw err;
    }
    return data;
  }
  throw new Error(`Discord no respondió: ${method} ${path}`);
}

// ---------------------------------------------------------------- memoria por servidor
// Los datos se guardan como un archivo en un canal privado del propio servidor,
// así no se pierden cuando Render reinicia.
const servidores = new Map(); // guildId -> { data, canal, cargando, timer, guardando }

function datosVacios() {
  return { guardados: [], puntos: {}, ganados: {}, torneo: null, avisos: [], sigAviso: 1, historial: [] };
}

async function canalMemoria(guildId) {
  const s = servidores.get(guildId);
  if (s && s.canal) return s.canal;
  const canales = await rest("GET", `/guilds/${guildId}/channels`);
  let c = canales.find((x) => x.type === 0 && x.name === CANAL_MEMORIA);
  if (!c) {
    c = await rest("POST", `/guilds/${guildId}/channels`, {
      name: CANAL_MEMORIA,
      type: 0,
      topic: "Aquí Alvin guarda su memoria. No borres este canal.",
      permission_overwrites: [
        { id: guildId, type: 0, deny: String(1 << 10) },
        { id: BOT_ID, type: 1, allow: String((1 << 10) | (1 << 11) | (1 << 15) | (1 << 16) | (1 << 13)) },
      ],
    });
  }
  return c.id;
}

async function cargar(guildId) {
  let s = servidores.get(guildId);
  if (s && s.data) return s.data;
  if (s && s.cargando) return s.cargando;
  s = s || {};
  servidores.set(guildId, s);
  s.cargando = (async () => {
    let data = datosVacios();
    try {
      s.canal = await canalMemoria(guildId);
      const msgs = await rest("GET", `/channels/${s.canal}/messages?limit=20`);
      const mio = msgs.find((x) => x.author && x.author.id === BOT_ID && x.attachments && x.attachments.some((a) => a.filename === "alvin.json"));
      if (mio) {
        const url = mio.attachments.find((a) => a.filename === "alvin.json").url;
        const r = await fetch(url);
        if (r.ok) data = Object.assign(datosVacios(), await r.json());
      }
    } catch (e) {
      log("no pude cargar la memoria de", guildId, e.message);
    }
    s.data = data;
    s.cargando = null;
    return data;
  })();
  return s.cargando;
}

function guardar(guildId) {
  const s = servidores.get(guildId);
  if (!s || !s.data) return;
  clearTimeout(s.timer);
  s.timer = setTimeout(() => escribir(guildId), 3000);
}

async function escribir(guildId) {
  const s = servidores.get(guildId);
  if (!s || !s.data) return;
  if (s.guardando) { s.otraVez = true; return; }
  s.guardando = true;
  try {
    if (!s.canal) s.canal = await canalMemoria(guildId);
    const nuevo = await rest("POST", `/channels/${s.canal}/messages`, { content: `🧠 Memoria de Alvin (${new Date().toISOString()})` }, [
      { name: "alvin.json", data: JSON.stringify(s.data) },
    ]);
    // borra memorias viejas
    const msgs = await rest("GET", `/channels/${s.canal}/messages?limit=20`);
    for (const x of msgs) {
      if (x.id !== nuevo.id && x.author && x.author.id === BOT_ID) {
        await rest("DELETE", `/channels/${s.canal}/messages/${x.id}`).catch(() => {});
      }
    }
  } catch (e) {
    log("no pude guardar la memoria de", guildId, e.message);
    if (e.status === 404) s.canal = null;
  }
  s.guardando = false;
  if (s.otraVez) { s.otraVez = false; guardar(guildId); }
}

function recordar(data, ids) {
  let cambio = false;
  for (const id of ids) {
    if (!id || id === BOT_ID) continue;
    if (!data.guardados.includes(id)) { data.guardados.push(id); cambio = true; }
  }
  return cambio;
}

// ---------------------------------------------------------------- personalidad
const FRASES = {
  hola: ["¿Qué fue, wey? Aquí andamos 😎", "¡Quiubo! Alvin presente, como siempre.", "Holaaa, ¿qué se arma hoy?", "¿Me llamaron? Ya llegó el más wey del server."],
  gracias: ["De nada, wey, para eso estoy 😌", "Tranqui, invítame un cola después.", "Cuando quieras, ñaño."],
  insulto: ["Uy, qué agresivo… igual te quiero, wey 💅", "Eso dolió más que perder en la final.", "Te voy a bajar 5 puntos del ranking… nah, mentira. ¿O no? 👀"],
  quien: ["Soy Alvin, el wey oficial del server: ruletas, torneos, avisos y mucho flow.", "Alvin, para servirte. Escribe /ayuda y te enseño todo lo que sé hacer."],
  aburrido: ["¿Aburrido? Hagamos un /torneo, wey.", "Tira una /ruleta y que el destino decida 🎰", "Ponte a jugar algo, yo arbitro."],
  nada: ["¿Sí, wey? Dime.", "Aquí estoy, ¿qué necesitas?", "Me mencionaste y vine corriendo 🏃", "Habla, que te escucho.", "Escribe /ayuda si no sabes qué pedirme, wey."],
};
const BOLA = [
  "Sí, obvio, wey.", "Ni lo sueñes 😂", "Pregúntame después de comer.", "Clarísimo que sí.", "Mmm… no creo.",
  "Las estrellas dicen que sí ✨", "Eso ni Goku lo sabe.", "100% seguro.", "Ni en mil años.", "Puede ser, puede ser 👀",
  "Ahorita no, joven.", "Sí, pero no le digas a nadie.", "Mi respuesta es: tal vez.", "Eso está más difícil que ganarle a Alvin.",
];

function responderMencion(texto) {
  const t = texto.toLowerCase();
  if (/\b(hola|buenas|que fue|qué fue|quiubo|holi|hey)\b/.test(t)) return pick(FRASES.hola);
  if (/gracias|grax|thx/.test(t)) return pick(FRASES.gracias);
  if (/(tonto|feo|bobo|idiota|malo|pelotudo|wey de mierda|basura)/.test(t)) return pick(FRASES.insulto);
  if (/(qui[eé]n eres|que eres|qué eres|qué haces|que haces)/.test(t)) return pick(FRASES.quien);
  if (/aburr/.test(t)) return pick(FRASES.aburrido);
  if (/\?\s*$/.test(t)) return "🎱 " + pick(BOLA);
  return pick(FRASES.nada);
}

// ---------------------------------------------------------------- fechas (hora de Ecuador)
function ahoraEc() {
  const d = new Date(Date.now() + ZONA * 3600e3);
  return { y: d.getUTCFullYear(), mo: d.getUTCMonth() + 1, d: d.getUTCDate() };
}
function parseFecha(txt) {
  txt = String(txt || "").trim().toLowerCase();
  const h = ahoraEc();
  if (!txt || txt === "hoy") return h;
  if (txt === "mañana" || txt === "manana") {
    const d = new Date(Date.UTC(h.y, h.mo - 1, h.d + 1));
    return { y: d.getUTCFullYear(), mo: d.getUTCMonth() + 1, d: d.getUTCDate() };
  }
  const x = txt.match(/^(\d{1,2})[\/\-.](\d{1,2})(?:[\/\-.](\d{2,4}))?$/);
  if (!x) return null;
  let y = x[3] ? +x[3] : h.y;
  if (y < 100) y += 2000;
  const r = { y, mo: +x[2], d: +x[1] };
  if (r.mo < 1 || r.mo > 12 || r.d < 1 || r.d > 31) return null;
  // si no puso año y la fecha ya pasó, es el próximo año
  if (!x[3] && Date.UTC(r.y, r.mo - 1, r.d) < Date.UTC(h.y, h.mo - 1, h.d)) r.y++;
  return r;
}
function parseHora(txt) {
  txt = String(txt || "").trim().toLowerCase().replace(/\s+/g, "").replace(/\./g, "");
  const x = txt.match(/^(\d{1,2})(?::(\d{2}))?(am|pm)?$/);
  if (!x) return null;
  let h = +x[1];
  const mi = x[2] ? +x[2] : 0;
  if (x[3] === "pm" && h < 12) h += 12;
  if (x[3] === "am" && h === 12) h = 0;
  if (h > 23 || mi > 59) return null;
  return { h, mi };
}
function cuando(f, h) {
  return Date.UTC(f.y, f.mo - 1, f.d, h.h - ZONA, h.mi);
}
const ts = (ms, estilo = "F") => `<t:${Math.floor(ms / 1000)}:${estilo}>`;

// ---------------------------------------------------------------- comandos
const OPC = { SUB: 1, STR: 3, INT: 4, USER: 6, CHANNEL: 7, ROLE: 8 };
const MENCION_CHOICES = [
  { name: "A todos (@everyone)", value: "everyone" },
  { name: "A los conectados (@here)", value: "here" },
  { name: "A nadie", value: "nadie" },
];
const COMANDOS = [
  {
    name: "ruleta",
    description: "Gira la ruleta y escoge a uno al azar",
    options: [
      { type: OPC.STR, name: "personas", description: "Escribe los @ de los que entran (vacío = todos los guardados)" },
      { type: OPC.STR, name: "premio", description: "¿Qué se gana (o qué castigo)?" },
      { type: OPC.INT, name: "cuantos", description: "Cuántos escoger (1 por defecto)", min_value: 1, max_value: 10 },
    ],
  },
  {
    name: "emparejar",
    description: "Arma parejas o equipos al azar",
    options: [
      { type: OPC.STR, name: "personas", description: "Escribe los @ (vacío = todos los guardados)" },
      { type: OPC.INT, name: "tamaño", description: "Personas por equipo (2 = parejas)", min_value: 2, max_value: 10 },
    ],
  },
  {
    name: "guardados",
    description: "La lista de @ que Alvin recuerda",
    options: [
      { type: OPC.SUB, name: "ver", description: "Ver a todos los guardados" },
      { type: OPC.SUB, name: "agregar", description: "Guardar a varios @", options: [{ type: OPC.STR, name: "personas", description: "Escribe los @", required: true }] },
      { type: OPC.SUB, name: "quitar", description: "Quitar a alguien", options: [{ type: OPC.USER, name: "jugador", description: "¿A quién?", required: true }] },
    ],
  },
  {
    name: "torneo",
    description: "Torneos con llaves, ganadores y puntos",
    options: [
      { type: OPC.SUB, name: "crear", description: "Crear un torneo nuevo", options: [{ type: OPC.STR, name: "nombre", description: "Nombre del torneo", required: true }] },
      { type: OPC.SUB, name: "unirse", description: "Entrar al torneo" },
      { type: OPC.SUB, name: "agregar", description: "Meter a varios @ al torneo", options: [{ type: OPC.STR, name: "personas", description: "Escribe los @ (o 'todos' para los guardados)", required: true }] },
      { type: OPC.SUB, name: "quitar", description: "Sacar a alguien antes de empezar", options: [{ type: OPC.USER, name: "jugador", description: "¿A quién?", required: true }] },
      { type: OPC.SUB, name: "empezar", description: "Sortear las llaves y empezar" },
      { type: OPC.SUB, name: "ganador", description: "Marcar quién ganó su partida", options: [{ type: OPC.USER, name: "jugador", description: "El que ganó", required: true }] },
      { type: OPC.SUB, name: "ver", description: "Ver el torneo y las llaves" },
      { type: OPC.SUB, name: "cancelar", description: "Cancelar el torneo" },
    ],
  },
  { name: "ranking", description: "La tabla de puntos del server" },
  {
    name: "puntos",
    description: "Dar o quitar puntos (solo admins)",
    options: [
      { type: OPC.USER, name: "jugador", description: "¿A quién?", required: true },
      { type: OPC.INT, name: "cantidad", description: "Puntos (negativo para quitar)", required: true, min_value: -1000, max_value: 1000 },
      { type: OPC.STR, name: "razon", description: "¿Por qué?" },
    ],
  },
  {
    name: "aviso",
    description: "Avisos importantes (solo admins)",
    options: [
      {
        type: OPC.SUB, name: "ahora", description: "Mandar un aviso ya mismo",
        options: [
          { type: OPC.STR, name: "titulo", description: "Título del aviso", required: true },
          { type: OPC.STR, name: "mensaje", description: "Lo que quieres decir", required: true },
          { type: OPC.STR, name: "mencionar", description: "¿A quién avisar?", choices: MENCION_CHOICES },
          { type: OPC.ROLE, name: "rol", description: "O avisarle a un rol" },
        ],
      },
      {
        type: OPC.SUB, name: "programar", description: "Programar un evento con fecha y hora",
        options: [
          { type: OPC.STR, name: "titulo", description: "Nombre del evento", required: true },
          { type: OPC.STR, name: "fecha", description: "hoy, mañana o 25/12", required: true },
          { type: OPC.STR, name: "hora", description: "Ejemplo: 20:30 o 8:30pm (hora de Ecuador)", required: true },
          { type: OPC.STR, name: "mensaje", description: "Detalles del evento" },
          { type: OPC.STR, name: "mencionar", description: "¿A quién avisar?", choices: MENCION_CHOICES },
          { type: OPC.ROLE, name: "rol", description: "O avisarle a un rol" },
          { type: OPC.CHANNEL, name: "canal", description: "Dónde avisar (por defecto aquí)", channel_types: [0, 5] },
        ],
      },
      { type: OPC.SUB, name: "lista", description: "Ver los eventos programados" },
      { type: OPC.SUB, name: "borrar", description: "Borrar un evento programado", options: [{ type: OPC.INT, name: "numero", description: "Número del evento (míralo en /aviso lista)", required: true }] },
    ],
  },
  { name: "alvin", description: "Pregúntale algo a Alvin", options: [{ type: OPC.STR, name: "pregunta", description: "¿Qué le quieres preguntar?", required: true }] },
  { name: "ayuda", description: "Todo lo que sabe hacer Alvin" },
];

// ---------------------------------------------------------------- respuestas a interacciones
async function responder(i, data, efimero) {
  if (typeof data === "string") data = { content: data };
  if (efimero) data.flags = 64;
  data.allowed_mentions = data.allowed_mentions || { parse: [] };
  await rest("POST", `/interactions/${i.id}/${i.token}/callback`, { type: 4, data });
}
async function editar(i, data) {
  if (typeof data === "string") data = { content: data };
  data.allowed_mentions = data.allowed_mentions || { parse: [] };
  await rest("PATCH", `/webhooks/${APP_ID}/${i.token}/messages/@original`, data);
}
function opciones(i) {
  let o = (i.data && i.data.options) || [];
  let sub = null;
  if (o[0] && o[0].type === OPC.SUB) { sub = o[0].name; o = o[0].options || []; }
  const v = {};
  for (const x of o) v[x.name] = x.value;
  return { sub, v };
}
function esAdmin(i) {
  const p = BigInt((i.member && i.member.permissions) || "0");
  return (p & 0x8n) !== 0n || (p & 0x20n) !== 0n;
}
const embed = (title, description, extra = {}) => ({ embeds: [Object.assign({ title, description, color: COLOR }, extra)] });

// ---- ruleta
async function cmdRuleta(i, data) {
  const { v } = opciones(i);
  let gente = sacarMenciones(v.personas);
  if (recordar(data, gente)) guardar(i.guild_id);
  if (!gente.length) gente = data.guardados.slice();
  const cuantos = Math.min(v.cuantos || 1, gente.length);
  if (gente.length < 2) {
    return responder(i, "🎰 Necesito por lo menos 2 personas, wey. Escribe sus @ en `personas` o guarda gente con `/guardados agregar`.", true);
  }
  const premio = v.premio ? `\n🎁 **Premio:** ${v.premio}` : "";
  await responder(i, embed("🎰 ¡Girando la ruleta!", `Participan: ${gente.map(m).join(" ")}${premio}\n\n⏳ ...`));
  for (let k = 0; k < 4; k++) {
    await sleep(900);
    const paso = ["🔄", "🌀", "💫", "⚡"][k];
    await editar(i, embed("🎰 ¡Girando la ruleta!", `Participan: ${gente.map(m).join(" ")}${premio}\n\n${paso} ${m(pick(gente))}...`)).catch(() => {});
  }
  await sleep(900);
  const elegidos = shuffle(gente).slice(0, cuantos);
  const frase = pick(["¡El destino habló, wey!", "No hay reclamos, la ruleta no miente.", "¡Qué suerte (o qué mala suerte) 😂!", "Alvin lo decidió así."]);
  await editar(i, {
    content: elegidos.map(m).join(" "),
    allowed_mentions: { users: elegidos },
    ...embed("🎯 ¡La ruleta escogió!", `${cuantos > 1 ? "Los elegidos son" : "El elegido es"}: ${elegidos.map(m).join(", ")}${premio}\n\n${frase}`),
  });
}

// ---- emparejar
function armarEquipos(gente, tam) {
  const g = shuffle(gente);
  let nEq = Math.max(1, Math.ceil(g.length / tam));
  if (nEq > 1 && Math.floor(g.length / nEq) < 2) nEq--;
  const eq = Array.from({ length: nEq }, () => []);
  g.forEach((id, k) => eq[k % nEq].push(id));
  return eq;
}
async function cmdEmparejar(i, data) {
  const { v } = opciones(i);
  let gente = sacarMenciones(v.personas);
  if (recordar(data, gente)) guardar(i.guild_id);
  if (!gente.length) gente = data.guardados.slice();
  const tam = v["tamaño"] || 2;
  if (gente.length < 2) return responder(i, "👥 Necesito por lo menos 2 personas, wey.", true);
  const eq = armarEquipos(gente, tam);
  const nombre = tam === 2 ? "Pareja" : "Equipo";
  const lineas = eq.map((e, k) => `**${nombre} ${k + 1}:** ${e.map(m).join(" + ")}`);
  await responder(i, embed(tam === 2 ? "👥 ¡Parejas listas!" : "👥 ¡Equipos listos!", lineas.join("\n") + "\n\n" + pick(["Nada de cambiarse, wey.", "Que gane el mejor 💪", "Alvin ya habló."])));
}

// ---- guardados
async function cmdGuardados(i, data) {
  const { sub, v } = opciones(i);
  if (sub === "agregar") {
    const ids = sacarMenciones(v.personas);
    if (!ids.length) return responder(i, "No vi ningún @, wey. Escríbelos así: @fulano @mengano", true);
    recordar(data, ids);
    guardar(i.guild_id);
    return responder(i, `📌 Guardé a ${ids.map(m).join(" ")}. Ya son **${data.guardados.length}** en mi lista.`);
  }
  if (sub === "quitar") {
    data.guardados = data.guardados.filter((x) => x !== v.jugador);
    guardar(i.guild_id);
    return responder(i, `🗑️ Saqué a ${m(v.jugador)} de mi lista.`);
  }
  if (!data.guardados.length) return responder(i, "📭 Todavía no tengo a nadie guardado. Usa `/guardados agregar`.", true);
  return responder(i, embed(`📌 Guardados (${data.guardados.length})`, data.guardados.map(m).join(" ")));
}

// ---- puntos y ranking
function sumar(data, id, n) {
  data.puntos[id] = (data.puntos[id] || 0) + n;
}
async function cmdPuntos(i, data) {
  if (!esAdmin(i)) return responder(i, "🚫 Solo los admins pueden dar puntos, wey.", true);
  const { v } = opciones(i);
  sumar(data, v.jugador, v.cantidad);
  recordar(data, [v.jugador]);
  guardar(i.guild_id);
  const signo = v.cantidad >= 0 ? `+${v.cantidad}` : `${v.cantidad}`;
  await responder(i, embed("📊 Puntos", `${m(v.jugador)} **${signo}** puntos${v.razon ? ` por: ${v.razon}` : ""}.\nAhora tiene **${data.puntos[v.jugador]}**.`));
}
async function cmdRanking(i, data) {
  const lista = Object.entries(data.puntos).filter(([, p]) => p !== 0).sort((a, b) => b[1] - a[1]).slice(0, 15);
  if (!lista.length) return responder(i, "📊 Todavía nadie tiene puntos. ¡Hagan un /torneo!", true);
  const med = ["🥇", "🥈", "🥉"];
  const txt = lista.map(([id, p], k) => `${med[k] || `**${k + 1}.**`} ${m(id)} — **${p}** pts${data.ganados[id] ? ` · 🏆×${data.ganados[id]}` : ""}`).join("\n");
  await responder(i, embed("📊 Ranking del server", txt));
}

// ---- torneo
function textoLlaves(t) {
  if (!t.rondas.length) {
    return `**Inscritos (${t.jugadores.length}):** ${t.jugadores.length ? t.jugadores.map(m).join(" ") : "nadie todavía"}\n\nPara entrar: \`/torneo unirse\``;
  }
  const out = [];
  t.rondas.forEach((r, k) => {
    const nombre = nombreRonda(r.length, k);
    out.push(`__**${nombre}**__`);
    for (const p of r) {
      if (!p.b) out.push(`• ${m(p.a)} pasa directo ✅`);
      else if (p.gana) out.push(`• ${p.gana === p.a ? `**${m(p.a)}** ✅` : m(p.a)} vs ${p.gana === p.b ? `**${m(p.b)}** ✅` : m(p.b)}`);
      else out.push(`• ${m(p.a)} vs ${m(p.b)} ⏳`);
    }
  });
  return out.join("\n");
}
function nombreRonda(nPartidas, k) {
  if (nPartidas === 1) return "🏆 Final";
  if (nPartidas === 2) return "Semifinal";
  if (nPartidas <= 4) return "Cuartos de final";
  return `Ronda ${k + 1}`;
}
function emparejarRonda(ids) {
  const g = shuffle(ids);
  const r = [];
  for (let k = 0; k < g.length; k += 2) {
    const p = { a: g[k], b: g[k + 1] || null, gana: null };
    if (!p.b) p.gana = p.a; // pasa directo
    r.push(p);
  }
  return r;
}
async function cmdTorneo(i, data) {
  const { sub, v } = opciones(i);
  const autor = (i.member && i.member.user && i.member.user.id) || (i.user && i.user.id);
  let t = data.torneo;
  const puedeMandar = () => esAdmin(i) || (t && t.creador === autor);

  if (sub === "crear") {
    if (t) return responder(i, `⚠️ Ya hay un torneo activo: **${t.nombre}**. Termínalo o usa \`/torneo cancelar\`.`, true);
    data.torneo = { nombre: String(v.nombre).slice(0, 80), creador: autor, jugadores: [], rondas: [], creado: Date.now() };
    guardar(i.guild_id);
    return responder(i, embed(`🏆 Nuevo torneo: ${data.torneo.nombre}`, `Lo creó ${m(autor)}.\n\nEntren con \`/torneo unirse\`, o el creador los mete con \`/torneo agregar\`.\nCuando estén todos: \`/torneo empezar\`.`));
  }
  if (!t) return responder(i, "🤷 No hay ningún torneo activo. Crea uno con `/torneo crear`.", true);

  if (sub === "unirse" || sub === "agregar") {
    if (t.rondas.length) return responder(i, "⛔ El torneo ya empezó, wey. Para la próxima.", true);
    let ids;
    if (sub === "unirse") ids = [autor];
    else {
      if (!puedeMandar()) return responder(i, "🚫 Solo el creador del torneo o un admin puede meter gente.", true);
      ids = /^\s*todos\s*$/i.test(v.personas) ? data.guardados.slice() : sacarMenciones(v.personas);
    }
    const nuevos = ids.filter((x) => x !== BOT_ID && !t.jugadores.includes(x));
    if (!nuevos.length) return responder(i, "Ya estaban todos adentro, wey.", true);
    t.jugadores.push(...nuevos);
    recordar(data, nuevos);
    guardar(i.guild_id);
    return responder(i, `✅ Entraron a **${t.nombre}**: ${nuevos.map(m).join(" ")}\nYa son **${t.jugadores.length}** jugadores.`);
  }
  if (sub === "quitar") {
    if (!puedeMandar()) return responder(i, "🚫 Solo el creador del torneo o un admin.", true);
    if (t.rondas.length) return responder(i, "⛔ Ya empezó, no se puede sacar a nadie.", true);
    t.jugadores = t.jugadores.filter((x) => x !== v.jugador);
    guardar(i.guild_id);
    return responder(i, `🗑️ Saqué a ${m(v.jugador)} del torneo.`);
  }
  if (sub === "empezar") {
    if (!puedeMandar()) return responder(i, "🚫 Solo el creador del torneo o un admin puede empezarlo.", true);
    if (t.rondas.length) return responder(i, "Ya empezó, wey. Mira las llaves con `/torneo ver`.", true);
    if (t.jugadores.length < 2) return responder(i, "Necesito por lo menos 2 jugadores.", true);
    t.rondas.push(emparejarRonda(t.jugadores));
    guardar(i.guild_id);
    return responder(i, {
      content: t.jugadores.map(m).join(" "),
      allowed_mentions: { users: t.jugadores.slice(0, 100) },
      ...embed(`🔥 ¡Empieza ${t.nombre}!`, textoLlaves(t) + "\n\nCuando alguien gane: `/torneo ganador @jugador`"),
    });
  }
  if (sub === "ganador") {
    if (!puedeMandar()) return responder(i, "🚫 Solo el creador del torneo o un admin marca ganadores.", true);
    if (!t.rondas.length) return responder(i, "El torneo todavía no empieza. Usa `/torneo empezar`.", true);
    const ronda = t.rondas[t.rondas.length - 1];
    const p = ronda.find((x) => !x.gana && (x.a === v.jugador || x.b === v.jugador));
    if (!p) return responder(i, `No encontré una partida pendiente de ${m(v.jugador)} en esta ronda.`, true);
    p.gana = v.jugador;
    sumar(data, v.jugador, 2);
    let extra = `✅ ${m(v.jugador)} ganó su partida (+2 pts).`;
    if (ronda.every((x) => x.gana)) {
      const vivos = ronda.map((x) => x.gana);
      if (vivos.length === 1) {
        // fin del torneo
        const campeon = vivos[0];
        const final = ronda[0];
        const sub2 = final.a === campeon ? final.b : final.a;
        sumar(data, campeon, 10);
        if (sub2) sumar(data, sub2, 5);
        for (const j of t.jugadores) sumar(data, j, 1);
        data.ganados[campeon] = (data.ganados[campeon] || 0) + 1;
        data.historial.unshift({ nombre: t.nombre, campeon, fecha: Date.now() });
        data.historial = data.historial.slice(0, 30);
        const llaves = textoLlaves(t);
        data.torneo = null;
        guardar(i.guild_id);
        return responder(i, {
          content: m(campeon),
          allowed_mentions: { users: [campeon] },
          ...embed(`👑 ¡${m(campeon)} es el campeón de ${t.nombre}!`, `${llaves}\n\n🏆 Campeón: ${m(campeon)} **+10 pts**\n🥈 Subcampeón: ${sub2 ? m(sub2) : "—"} **+5 pts**\n🎖️ Todos los participantes **+1 pt**\n\n${pick(["¡Qué nivel, wey!", "Leyenda absoluta 🔥", "Nadie lo pudo parar."])}`),
        });
      }
      t.rondas.push(emparejarRonda(vivos));
      extra += `\n\n➡️ ¡Ronda terminada! Nueva ronda: **${nombreRonda(t.rondas[t.rondas.length - 1].length, t.rondas.length - 1)}**`;
    }
    guardar(i.guild_id);
    return responder(i, embed(`🏆 ${t.nombre}`, extra + "\n\n" + textoLlaves(t)));
  }
  if (sub === "cancelar") {
    if (!puedeMandar()) return responder(i, "🚫 Solo el creador del torneo o un admin.", true);
    data.torneo = null;
    guardar(i.guild_id);
    return responder(i, `❌ Cancelé el torneo **${t.nombre}**.`);
  }
  return responder(i, embed(`🏆 ${t.nombre}`, textoLlaves(t)));
}

// ---- avisos
function mencionDe(v) {
  if (v.rol) return { texto: `<@&${v.rol}>`, allowed: { roles: [v.rol] } };
  if (v.mencionar === "everyone") return { texto: "@everyone", allowed: { parse: ["everyone"] } };
  if (v.mencionar === "here") return { texto: "@here", allowed: { parse: ["everyone"] } };
  return { texto: "", allowed: { parse: [] } };
}
async function cmdAviso(i, data) {
  const { sub, v } = opciones(i);
  const autor = (i.member && i.member.user && i.member.user.id) || "";
  if (sub !== "lista" && !esAdmin(i)) return responder(i, "🚫 Solo los admins pueden mandar avisos, wey.", true);

  if (sub === "ahora") {
    const men = mencionDe(v);
    return responder(i, {
      content: men.texto || undefined,
      allowed_mentions: men.allowed,
      embeds: [{ title: `📢 ${v.titulo}`, description: v.mensaje, color: COLOR, footer: { text: "Aviso de Alvin" } }],
    });
  }
  if (sub === "programar") {
    const f = parseFecha(v.fecha);
    const h = parseHora(v.hora);
    if (!f) return responder(i, "📅 No entendí la fecha. Usa `hoy`, `mañana` o algo como `25/12`.", true);
    if (!h) return responder(i, "⏰ No entendí la hora. Usa algo como `20:30` o `8:30pm`.", true);
    const when = cuando(f, h);
    if (when < Date.now() + 30e3) return responder(i, "⏪ Esa hora ya pasó, wey.", true);
    const men = mencionDe(v);
    const a = {
      id: data.sigAviso++,
      titulo: String(v.titulo).slice(0, 150),
      mensaje: v.mensaje ? String(v.mensaje).slice(0, 1500) : "",
      canal: v.canal || i.channel_id,
      cuando: when,
      mencion: men.texto,
      allowed: men.allowed,
      autor,
      recordado: when - Date.now() < 65 * 60e3, // si falta menos de una hora, no hay recordatorio previo
      enviado: false,
    };
    data.avisos.push(a);
    guardar(i.guild_id);
    return responder(i, embed(`📅 Evento programado #${a.id}: ${a.titulo}`, `Cuándo: ${ts(when)} (${ts(when, "R")})\nDónde aviso: <#${a.canal}>\n${a.recordado ? "" : "Aviso **1 hora antes** y "}cuando empiece.${a.mensaje ? `\n\n${a.mensaje}` : ""}`));
  }
  if (sub === "borrar") {
    const antes = data.avisos.length;
    data.avisos = data.avisos.filter((a) => a.id !== v.numero);
    if (data.avisos.length === antes) return responder(i, `No encontré el evento #${v.numero}.`, true);
    guardar(i.guild_id);
    return responder(i, `🗑️ Borré el evento #${v.numero}.`);
  }
  const pend = data.avisos.filter((a) => !a.enviado).sort((a, b) => a.cuando - b.cuando);
  if (!pend.length) return responder(i, "📭 No hay eventos programados.", true);
  return responder(i, embed("📅 Eventos programados", pend.map((a) => `**#${a.id}** ${a.titulo} — ${ts(a.cuando)} (${ts(a.cuando, "R")}) en <#${a.canal}>`).join("\n")));
}

async function revisarAvisos() {
  const ahora = Date.now();
  for (const [gid, s] of servidores) {
    const data = s.data;
    if (!data || !data.avisos.length) continue;
    let cambio = false;
    for (const a of data.avisos) {
      if (a.enviado) continue;
      try {
        if (!a.recordado && ahora >= a.cuando - 60 * 60e3) {
          a.recordado = true;
          cambio = true;
          await rest("POST", `/channels/${a.canal}/messages`, {
            content: a.mencion || undefined,
            allowed_mentions: a.allowed || { parse: [] },
            embeds: [{ title: `⏰ En 1 hora: ${a.titulo}`, description: `Empieza ${ts(a.cuando, "R")} (${ts(a.cuando, "t")}).${a.mensaje ? `\n\n${a.mensaje}` : ""}`, color: COLOR }],
          });
        }
        if (ahora >= a.cuando) {
          a.enviado = true;
          cambio = true;
          await rest("POST", `/channels/${a.canal}/messages`, {
            content: a.mencion || undefined,
            allowed_mentions: a.allowed || { parse: [] },
            embeds: [{ title: `🚨 ¡Ya empieza: ${a.titulo}!`, description: (a.mensaje || "") + `\n\n${pick(["¡Vamos, weyes!", "¡A darle!", "Nadie se quede afuera 🔥"])}`, color: COLOR }],
          });
        }
      } catch (e) {
        log("aviso fallido", gid, a.id, e.message);
        if (e.status === 403 || e.status === 404) { a.enviado = true; cambio = true; }
      }
    }
    // limpia avisos viejos ya enviados
    const antes = data.avisos.length;
    data.avisos = data.avisos.filter((a) => !a.enviado || ahora - a.cuando < 24 * 3600e3);
    if (cambio || data.avisos.length !== antes) guardar(gid);
  }
}

// ---- alvin y ayuda
async function cmdAlvin(i) {
  const { v } = opciones(i);
  const p = String(v.pregunta).slice(0, 300);
  await responder(i, embed("🎱 Alvin responde", `**Pregunta:** ${p}\n**Respuesta:** ${pick(BOLA)}`));
}
async function cmdAyuda(i) {
  await responder(i, embed("😎 Soy Alvin, el wey del server", [
    "🎰 `/ruleta` — escribe varios @ y escojo uno (o varios) al azar.",
    "👥 `/emparejar` — armo parejas o equipos al azar.",
    "📌 `/guardados` — guardo los @ del grupo para no escribirlos siempre.",
    "🏆 `/torneo` — crear, unirse, empezar, marcar ganadores y ver las llaves.",
    "📊 `/ranking` y `/puntos` — la tabla de puntos (el campeón gana +10).",
    "📢 `/aviso` — avisos al instante o eventos con fecha y hora (aviso 1 hora antes).",
    "🎱 `/alvin` — pregúntame lo que quieras.",
    "💬 Y si me mencionas, te contesto 😏",
  ].join("\n")));
}

const MANEJADORES = {
  ruleta: cmdRuleta, emparejar: cmdEmparejar, guardados: cmdGuardados, torneo: cmdTorneo,
  ranking: cmdRanking, puntos: cmdPuntos, aviso: cmdAviso, alvin: cmdAlvin, ayuda: cmdAyuda,
};

async function onInteraccion(i) {
  if (i.type !== 2 || !i.data) return;
  const fn = MANEJADORES[i.data.name];
  if (!fn) return;
  if (!i.guild_id) return responder(i, "Solo funciono dentro de un servidor, wey.", true).catch(() => {});
  try {
    const data = await cargar(i.guild_id);
    const autor = i.member && i.member.user && i.member.user.id;
    if (autor && recordar(data, [autor])) guardar(i.guild_id);
    await fn(i, data);
  } catch (e) {
    log("error en /" + i.data.name, e.message);
    await responder(i, "😵 Algo me falló, wey. Inténtalo otra vez.", true).catch(() => {});
  }
}

async function onMensaje(msg) {
  if (!msg || !msg.author || msg.author.bot || !msg.guild_id) return;
  const mencionado = (msg.mentions || []).some((u) => u.id === BOT_ID);
  if (!mencionado) return;
  const texto = String(msg.content || "").replace(/<@!?\d+>/g, "").trim();
  await rest("POST", `/channels/${msg.channel_id}/messages`, {
    content: responderMencion(texto),
    message_reference: { message_id: msg.id, fail_if_not_exists: false },
    allowed_mentions: { parse: [], replied_user: false },
  }).catch((e) => log("no pude responder mención", e.message));
}

// ---------------------------------------------------------------- gateway
const INTENTS = (1 << 0) | (1 << 9); // GUILDS + GUILD_MESSAGES (el texto llega cuando mencionan al bot)
let ws = null, seq = null, sesion = null, urlReanudar = null, latido = null, ackOk = true, detenido = false, reintentos = 0;

function enviar(op, d) {
  if (ws && ws.readyState === 1) ws.send(JSON.stringify({ op, d }));
}

async function conectar() {
  if (detenido) return;
  let url = urlReanudar;
  if (!url) {
    try {
      url = (await rest("GET", "/gateway/bot")).url;
    } catch (e) {
      log("no pude pedir el gateway:", e.message);
      if (e.status === 401) { detenido = true; log("TOKEN inválido. Revisa DISCORD_TOKEN."); return; }
      return setTimeout(conectar, 15000);
    }
  }
  ws = new WebSocket(`${url}/?v=10&encoding=json`);
  ws.onmessage = (ev) => {
    let p;
    try { p = JSON.parse(typeof ev.data === "string" ? ev.data : Buffer.from(ev.data).toString()); } catch (_) { return; }
    if (p.s != null) seq = p.s;
    if (p.op === 10) {
      clearInterval(latido);
      ackOk = true;
      const cada = p.d.heartbeat_interval;
      setTimeout(() => enviar(1, seq), Math.floor(cada * Math.random()));
      latido = setInterval(() => {
        if (!ackOk) { try { ws.close(4000); } catch (_) {} return; }
        ackOk = false;
        enviar(1, seq);
      }, cada);
      if (sesion && urlReanudar) enviar(6, { token: TOKEN, session_id: sesion, seq });
      else enviar(2, { token: TOKEN, intents: INTENTS, properties: { os: "linux", browser: "alvin", device: "alvin" }, presence: { status: "online", afk: false, activities: [{ name: "ser el más wey del server", type: 0 }] } });
    } else if (p.op === 11) {
      ackOk = true;
    } else if (p.op === 1) {
      enviar(1, seq);
    } else if (p.op === 7) {
      try { ws.close(4001); } catch (_) {}
    } else if (p.op === 9) {
      sesion = null; urlReanudar = null; seq = null;
      setTimeout(() => { try { ws.close(4002); } catch (_) {} }, 1500);
    } else if (p.op === 0) {
      evento(p.t, p.d);
    }
  };
  ws.onclose = (ev) => {
    clearInterval(latido);
    const code = ev && ev.code;
    if ([4004, 4010, 4011, 4012, 4013, 4014].includes(code)) {
      detenido = true;
      log("Discord cerró la conexión con código", code, code === 4004 ? "(TOKEN inválido)" : code === 4014 ? "(intents no permitidos)" : "");
      return;
    }
    if ([4007, 4009].includes(code)) { sesion = null; urlReanudar = null; seq = null; }
    reintentos++;
    setTimeout(conectar, Math.min(30000, 2000 * reintentos));
  };
  ws.onerror = () => {};
}

async function evento(t, d) {
  try {
    if (t === "READY") {
      reintentos = 0;
      sesion = d.session_id;
      urlReanudar = d.resume_gateway_url;
      BOT_ID = d.user.id;
      APP_ID = d.application.id;
      log(`conectado como ${d.user.username} en ${d.guilds.length} servidor(es)`);
      try {
        await rest("PUT", `/applications/${APP_ID}/commands`, COMANDOS);
        log("comandos registrados");
      } catch (e) {
        log("no pude registrar comandos:", e.message);
      }
    } else if (t === "RESUMED") {
      reintentos = 0;
      log("conexión reanudada");
    } else if (t === "GUILD_CREATE") {
      cargar(d.id).catch(() => {});
    } else if (t === "INTERACTION_CREATE") {
      await onInteraccion(d);
    } else if (t === "MESSAGE_CREATE") {
      await onMensaje(d);
    }
  } catch (e) {
    log("error en evento", t, e.message);
  }
}

let relojAvisos = null;
function start() {
  TOKEN = (process.env.DISCORD_TOKEN || "").trim();
  if (!TOKEN) { log("sin DISCORD_TOKEN: Alvin está apagado"); return false; }
  if (typeof WebSocket !== "function") { log("este Node no tiene WebSocket; necesito Node 22 o más nuevo"); return false; }
  process.on("unhandledRejection", (e) => log("error sin atrapar:", e && e.message));
  conectar();
  relojAvisos = setInterval(() => revisarAvisos().catch((e) => log("avisos:", e.message)), 20000);
  return true;
}
function stop() {
  detenido = true;
  clearInterval(relojAvisos);
  clearInterval(latido);
  try { ws && ws.close(1000); } catch (_) {}
}

module.exports = {
  start, stop,
  // para pruebas
  _t: { revisarAvisos: () => revisarAvisos(), escribir: (g) => escribir(g), parseFecha, parseHora, cuando, sacarMenciones, armarEquipos, emparejarRonda, responderMencion, COMANDOS, setApi: (u) => { module.exports._api = u; } },
};
