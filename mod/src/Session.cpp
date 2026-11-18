#include "Session.hpp"
#include "Net.hpp"
#include "Invites.hpp"

#include <Geode/binding/EditorUI.hpp>
#include <Geode/binding/GameLevelManager.hpp>
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/GJAccountManager.hpp>
#include <Geode/binding/LevelSettingsObject.hpp>
#include <Geode/binding/LocalLevelManager.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/UndoObject.hpp>
#include <Geode/binding/GameObjectCopy.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/utils/cocos.hpp>
#include <Geode/utils/random.hpp>
#include <Geode/utils/file.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace geode::prelude;

namespace collab {

// cada cuánto se revisan los cambios (segundos)
static constexpr float SCAN_INTERVAL = 0.25f;
// cuántos objetos "no seleccionados" se revisan por vuelta en niveles grandes
static constexpr size_t SCAN_CHUNK = 1000;
// si tienes muchísimos objetos seleccionados, se revisan por partes
static constexpr size_t SELECTED_FULL_LIMIT = 400;
// máximo de cambios por envío
static constexpr size_t MAX_OPS_PER_SEND = 3000;
static constexpr float CURSOR_INTERVAL = 0.1f;
static constexpr float PLAYER_INTERVAL = 1.f / 15.f;
static constexpr float SETTINGS_INTERVAL = 0.4f;
// segundos que la ventanita de invitación se queda en pantalla
static constexpr float ALERT_SECONDS = 15.f;
// objetos que se crean o revisan por frame al cargar una sala grande
static constexpr size_t LOAD_CHUNK = 2000;
static constexpr size_t MAX_SELECTION_SENT = 400;
static constexpr size_t MAX_SELECTION_DRAWN = 300;

static int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static void notify(std::string const& text, NotificationIcon icon = NotificationIcon::Info) {
    Notification::create(text, icon, 2.5f)->show();
}

Session& Session::get() {
    static Session instance;
    return instance;
}

void Session::init() {
    if (m_initialized) return;
    m_initialized = true;
    Net::get().onEvent = [this](matjson::Value const& ev) { this->onEvent(ev); };
    Net::get().onHello = [this](matjson::Value const& data, bool reconnect) { this->onHello(data, reconnect); };
}

// ------------------------------------------------------------------ escenas

// Se busca el nodo entre los hijos de la escena sin tocar el puntero,
// así no hay problema si ese editor ya se borró.
static bool sceneContains(CCScene* scene, CCNode* node) {
    if (!scene || !node || !scene->getChildren()) return false;
    for (auto child : CCArrayExt<CCNode*>(scene->getChildren())) {
        if (child == node) return true;
    }
    return false;
}

bool Session::inTransition() {
    return typeinfo_cast<CCTransitionScene*>(CCDirector::get()->getRunningScene()) != nullptr;
}

bool Session::editorInScene(LevelEditorLayer* editor) {
    if (!editor) return false;
    auto scene = CCDirector::get()->getRunningScene();
    if (auto t = typeinfo_cast<CCTransitionScene*>(scene)) {
        return sceneContains(t->m_pInScene, editor) || sceneContains(t->m_pOutScene, editor);
    }
    return sceneContains(scene, editor);
}

LevelEditorLayer* Session::currentEditor() {
    auto scene = CCDirector::get()->getRunningScene();
    if (!scene || typeinfo_cast<CCTransitionScene*>(scene)) return nullptr;
    return scene->getChildByType<LevelEditorLayer>(0);
}

bool Session::playingLevel() {
    auto scene = CCDirector::get()->getRunningScene();
    if (!scene) return false;
    if (auto t = typeinfo_cast<CCTransitionScene*>(scene)) {
        return (t->m_pInScene && t->m_pInScene->getChildByType<PlayLayer>(0))
            || (t->m_pOutScene && t->m_pOutScene->getChildByType<PlayLayer>(0));
    }
    return scene->getChildByType<PlayLayer>(0) != nullptr;
}

// ¿El nodo está dentro de la escena que se ve ahora?
static bool inRunningScene(CCNode* node) {
    if (!node) return false;
    auto scene = CCDirector::get()->getRunningScene();
    if (!scene || typeinfo_cast<CCTransitionScene*>(scene)) return false;
    CCNode* top = node;
    while (top->getParent()) top = top->getParent();
    return top == scene;
}

// ------------------------------------------------------------------ utilidades

LevelEditorLayer* Session::editor() const {
    if (!m_editor) return nullptr;
    // solo se usa si sigue en la escena (si no, pudo haberse borrado)
    if (!editorInScene(m_editor)) return nullptr;
    return m_editor;
}

bool Session::isInEditor(LevelEditorLayer* editor) const {
    return m_active && editor && editor == m_editor;
}

bool Session::isRoomLevel(GJGameLevel* level) const {
    return m_active && level && m_roomLevel && m_roomLevel.data() == level;
}

bool Session::isPlaytesting() const {
    auto ed = this->editor();
    return ed && ed->m_playbackMode != PlaybackMode::Not;
}

bool Session::isOwner() const {
    return m_active && m_ownerId == Net::get().clientId();
}

std::vector<Member const*> Session::members() const {
    std::vector<Member const*> out;
    for (auto const& [id, m] : m_members) out.push_back(&m);
    return out;
}

std::string Session::newObjectId() {
    return fmt::format("{}-{}", m_idPrefix, ++m_idCounter);
}

std::string Session::saveString(GameObject* obj) {
    std::string s = obj->getSaveString(m_editor);
    while (!s.empty() && (s.back() == ';' || s.back() == '\n')) s.pop_back();
    return s;
}

void Session::registerObject(std::string const& id, GameObject* obj, std::string const& str) {
    m_byId[id] = obj;
    m_byPtr[obj] = Tracked { id, str, m_scanTick };
}

void Session::forgetAllObjects() {
    m_byPtr.clear();
    m_byId.clear();
    m_pending.clear();
    m_outOps.clear();
    m_deferred.clear();
    m_held.clear();
    m_scanCursor = 0;
}

static std::string randomPrefix() {
    static const char* chars = "abcdefghijkmnpqrstuvwxyz23456789";
    std::string out;
    for (int i = 0; i < 5; i++) out += chars[geode::utils::random::generate<int>(0, 31)];
    return out;
}

static std::string levelHeader(LevelEditorLayer* editor) {
    std::string full = editor->getLevelString();
    if (full.rfind("H4sI", 0) == 0) {
        full = ZipUtils::decompressString(full, false, 0);
    }
    auto semi = full.find(';');
    return semi == std::string::npos ? full : full.substr(0, semi);
}

// ------------------------------------------------------------------ sala guardada

// Se recuerda en qué sala estás, por si cierras el juego y vuelves.
static void saveRoomInfo(std::string const& roomId, std::string const& localName) {
    Mod::get()->setSavedValue<std::string>("room-id", roomId);
    Mod::get()->setSavedValue<std::string>("room-level", localName);
}

static GJGameLevel* findLocalLevel(std::string const& name) {
    if (name.empty()) return nullptr;
    auto llm = LocalLevelManager::get();
    if (!llm || !llm->m_localLevels) return nullptr;
    for (auto level : CCArrayExt<GJGameLevel*>(llm->m_localLevels)) {
        if (std::string(level->m_levelName) == name) return level;
    }
    return nullptr;
}

void Session::onHello(matjson::Value const& data, bool reconnect) {
    bool serverHasRoom = data.contains("room") && data["room"].isObject();

    // invitaciones que te llegaron mientras no estabas
    int added = 0;
    for (auto const& iv : jarr(data, "invites")) {
        Invite inv;
        inv.roomId = jstr(iv, "roomId");
        inv.levelName = jstr(iv, "levelName");
        inv.fromName = iv.contains("from") ? jstr(iv["from"], "username") : "Alguien";
        if (inv.roomId.empty() || (m_active && inv.roomId == m_roomId)) continue;
        bool known = std::any_of(m_inbox.begin(), m_inbox.end(),
            [&](Invite const& i) { return i.roomId == inv.roomId; });
        if (!known) {
            m_inbox.push_back(inv);
            m_alertQueue.push_back(inv);
            added++;
        }
    }
    if (added > 0) {
        notify(fmt::format("Tienes {} invitación(es) Collab guardada(s) en tu perfil", added));
    }

    if (m_active) {
        if (!serverHasRoom) {
            // el servidor se reinició: se revive la sala con tu copia
            this->tryRevive();
            return;
        }
        // se cortó la conexión: pedir la copia de la sala otra vez
        m_sentState.clear();
        if (reconnect && m_editor) this->requestSnapshot(false);
        return;
    }
    if (m_joining) return;

    if (serverHasRoom) {
        // seguías en una sala (cerraste el juego o se cortó el internet)
        auto const& room = data["room"];
        this->becomeAway(jstr(room, "roomId"), jstr(room, "levelName"), jstr(room, "owner"), jarr(room, "members"));
        return;
    }

    auto saved = Mod::get()->getSavedValue<std::string>("room-id");
    if (!saved.empty()) {
        if (this->loadCache(saved)) {
            // la sala se perdió al reiniciarse el servidor: se revive con tu copia
            m_roomId = saved;
            this->tryRevive();
        } else {
            saveRoomInfo("", "");
            notify("Tu sala anterior se cerró. Tu copia del nivel sigue en tus niveles.", NotificationIcon::Warning);
        }
    }
}

void Session::becomeAway(std::string const& roomId, std::string const& levelName,
    std::string const& owner, std::vector<matjson::Value> const& members) {
    m_roomId = roomId;
    m_levelName = levelName;
    m_ownerId = owner;
    m_active = true;
    m_editor = nullptr;
    m_colorIndex = 0;
    this->setMembers(members);
    auto savedRoom = Mod::get()->getSavedValue<std::string>("room-id");
    auto savedLevel = Mod::get()->getSavedValue<std::string>("room-level");
    m_roomLevel = savedRoom == m_roomId ? findLocalLevel(savedLevel) : nullptr;
    m_sentState.clear();
    log::info("Collab: sigues en la sala {}", m_roomId);
    if (m_roomLevel) {
        notify(fmt::format("Sigues en la sala de \"{}\". Abre el nivel para seguir editando.", m_levelName));
    } else {
        notify(fmt::format("Sigues en la sala de \"{}\", pero no encontré tu copia del nivel.", m_levelName),
            NotificationIcon::Warning);
    }
}

// ------------------------------------------------------------------ crear sala

void Session::host(LevelEditorLayer* editor, std::function<void(bool, std::string)> done) {
    if (m_active) {
        if (m_editor == editor) done(true, "");
        else done(false, fmt::format("Ya estás en la sala de \"{}\". Sal de esa sala primero.", m_levelName));
        return;
    }
    if (m_busy || m_joining) {
        done(false, "Espera un momento...");
        return;
    }
    auto& net = Net::get();
    net.start();
    if (!net.isReady()) {
        done(false, net.lastError().empty()
            ? "Conectando con el servidor... intenta de nuevo en unos segundos."
            : net.lastError());
        return;
    }

    m_busy = true;
    this->forgetAllObjects();
    m_editor = editor;
    m_idPrefix = randomPrefix();
    m_idCounter = 0;

    auto objects = matjson::Value::array();
    for (auto obj : CCArrayExt<GameObject*>(editor->m_objects)) {
        auto id = this->newObjectId();
        auto str = this->saveString(obj);
        this->registerObject(id, obj, str);
        auto pair = matjson::Value::array();
        pair.push(id);
        pair.push(str);
        objects.push(std::move(pair));
    }

    auto body = matjson::Value::object();
    body["levelName"] = editor->m_level ? std::string(editor->m_level->m_levelName) : std::string("Nivel");
    m_lastHeader = this->settingsString(editor);
    m_lastMeta = this->metaString(editor);
    body["header"] = m_lastHeader.empty() ? levelHeader(editor) : m_lastHeader;
    body["meta"] = m_lastMeta;
    body["objects"] = std::move(objects);

    // niveles grandes tardan en subir
    net.call("/room/create", std::move(body), [this, editor, done](JsonResult res) {
        m_busy = false;
        if (!res) {
            this->forgetAllObjects();
            m_editor = nullptr;
            done(false, res.unwrapErr());
            return;
        }
        if (!editorInScene(editor)) {
            // cerraste el editor mientras se creaba
            this->forgetAllObjects();
            m_editor = nullptr;
            Net::get().call("/room/leave", matjson::Value::object(), nullptr);
            done(false, "Se cerró el editor.");
            return;
        }
        auto data = res.unwrap();
        m_roomId = jstr(data, "roomId");
        m_levelName = editor->m_level ? std::string(editor->m_level->m_levelName) : "";
        m_roomLevel = editor->m_level;
        m_ownerId = Net::get().clientId();
        m_active = true;
        m_members.clear();
        m_colorIndex = 0;
        m_sentState = "edit";
        auto me = matjson::Value::object();
        me["client"] = Net::get().clientId();
        me["accountID"] = GJAccountManager::sharedState()->m_accountID;
        me["username"] = std::string(GJAccountManager::sharedState()->m_username);
        me["state"] = "edit";
        this->addMember(me);
        saveRoomInfo(m_roomId, m_levelName);
        m_viewer = false;
        // copia local de la sala
        m_cacheRoom = m_roomId;
        m_cacheLevelName = m_levelName;
        m_cacheObjs.clear();
        for (auto& [ptr, info] : m_byPtr) m_cacheObjs[info.id] = info.str;
        m_cacheHeader = m_lastHeader;
        m_cacheMeta = m_lastMeta;
        m_cacheAllowed = { GJAccountManager::sharedState()->m_accountID };
        m_cacheViewers.clear();
        this->cacheTouch();
        log::info("Collab: sala {} creada", m_roomId);
        done(true, "");
    }, 240);
}

void Session::invite(int accountID, std::string const& username, std::function<void(bool, std::string)> done) {
    if (!m_active) {
        done(false, "Primero crea la sala.");
        return;
    }
    auto body = matjson::Value::object();
    if (accountID > 0) body["accountID"] = accountID;
    body["username"] = username;
    Net::get().call("/room/invite", std::move(body), [done](JsonResult res) {
        if (!res) {
            done(false, res.unwrapErr());
            return;
        }
        auto data = res.unwrap();
        auto name = jstr(data, "username");
        if (data.contains("busy") && data["busy"].asBool().unwrapOr(false)) {
            name += " (está en otra sala; le queda guardada)";
        } else if (data.contains("offline") && data["offline"].asBool().unwrapOr(false)) {
            name += " (no está conectado; le llegará al abrir el juego)";
        }
        done(true, name);
    });
}

// ------------------------------------------------------------------ unirse

void Session::accept(Invite const& inv) {
    if (m_active) {
        if (inv.roomId == m_roomId) {
            this->removeInvite(inv.roomId);
            notify("Ya estás en esa sala");
            return;
        }
        FLAlertLayer::create("Collab",
            fmt::format("Ya estás en la sala de <cg>{}</c>.\nPara unirte a otra, primero sal de esa "
                "(entra al nivel y toca el botón <cy>Collab</c> > <cr>Salir de la sala</c>).\n"
                "La invitación queda guardada en tu perfil.", m_levelName),
            "OK")->show();
        return;
    }
    if (currentEditor() || playingLevel()) {
        m_pendingJoin = inv;
        notify("Guarda y sal del nivel para entrar a la sala");
        return;
    }
    this->join(inv);
}

void Session::join(Invite const& invite) {
    if (m_busy || m_joining) return;
    if (m_active) return; // máximo una sala
    if (!Net::get().isReady()) {
        Net::get().start();
        m_pendingJoin = invite; // entra apenas se conecte
        notify("Conectando con el servidor... (si estaba dormido tarda ~1 minuto)");
        return;
    }
    m_busy = true;
    this->removeInvite(invite.roomId);

    auto body = matjson::Value::object();
    body["roomId"] = invite.roomId;
    body["state"] = "away";
    auto name = invite.levelName;

    Net::get().call("/room/join", std::move(body), [this, name, invite](JsonResult res) {
        m_busy = false;
        if (!res) {
            auto err = res.unwrapErr();
            // si la sala sigue existiendo, la invitación no se pierde
            if (err.find("no existe") == std::string::npos && err.find("invitación") == std::string::npos) {
                m_inbox.push_back(invite);
            }
            FLAlertLayer::create("Collab", err, "OK")->show();
            return;
        }
        if (playingLevel() || currentEditor()) {
            FLAlertLayer::create("Collab", "Sal del nivel antes de unirte.", "OK")->show();
            Net::get().call("/room/leave", matjson::Value::object(), nullptr);
            m_inbox.push_back(invite);
            return;
        }
        auto snap = res.unwrap();
        auto header = jstr(snap, "header");

        // se crea una copia local del nivel en "Mis niveles"
        auto level = GameLevelManager::sharedState()->createNewLevel();
        std::string levelName = "Collab " + name;
        if (levelName.size() > 20) levelName = levelName.substr(0, 20);
        level->m_levelName = levelName;
        level->m_levelString = ZipUtils::compressString(header + ";", false, 6, 6); // sin cifrar, compresión 6

        m_roomId = jstr(snap, "roomId");
        m_levelName = name;
        m_ownerId = jstr(snap, "owner");
        m_joinSnapshot = snap;
        m_joinLevel = level;
        this->cacheFromSnapshot(snap);
        m_joining = true;
        m_graceTimer = 0.f;
        m_earlyEvents.clear();
        saveRoomInfo(m_roomId, levelName);

        auto scene = LevelEditorLayer::scene(level, false);
        CCDirector::get()->replaceScene(CCTransitionFade::create(0.5f, scene));
    }, 120);
}

void Session::clearMemberNodes() {
    for (auto& [id, m] : m_members) {
        if (m.cursor) m.cursor->removeFromParent();
        if (m.ghost) m.ghost->removeFromParent();
        if (m.selDraw) m.selDraw->removeFromParent();
        m.hasCursor = false;
        m.ghostOn = false;
    }
    for (auto& mk : m_markers) {
        if (mk.node) mk.node->removeFromParent();
    }
    m_markers.clear();
}

bool Session::onEditorCreated(LevelEditorLayer* editor, GJGameLevel* level) {
    // entraste como invitado
    if (m_joinSnapshot && m_joinLevel && m_joinLevel.data() == level) {
        auto snap = std::move(*m_joinSnapshot);
        m_joinSnapshot.reset();
        m_joinLevel = nullptr;
        m_joining = false;

        this->forgetAllObjects();
        m_editor = editor;
        m_roomLevel = level;
        m_idPrefix = randomPrefix();
        m_idCounter = 0;
        m_active = true;
        m_colorIndex = 0;
        m_graceTimer = 0.f;
        m_exitTimer = 0.f;
        m_lastHeader.clear();
        m_lastMeta.clear();
        // los objetos se crean en el primer frame, con el editor ya listo
        m_snapshotToApply = std::move(snap);
        m_resyncRemoveUntracked = true;
        m_waitingSnapshot = true;
        return false;
    }

    // volviste a abrir el nivel de tu sala
    if (m_active && m_roomLevel && m_roomLevel.data() == level) {
        m_hostLevel = nullptr;
        this->clearMemberNodes();
        this->forgetAllObjects();
        m_editor = editor;
        m_idPrefix = randomPrefix();
        m_idCounter = 0;
        m_exitTimer = 0.f;
        m_lastHeader.clear();
        m_lastMeta.clear();
        m_bgIndex = m_mgIndex = m_groundIndex = m_lineIndex = -1;
        this->requestSnapshot(true);
        return false;
    }

    if (m_active) {
        // otro nivel: sigues en la sala, pero este editor no se comparte
        if (m_editor) this->detach(false);
        notify(fmt::format("Sigues en la sala de \"{}\". Este nivel no se comparte.", m_levelName));
        m_hostLevel = nullptr;
        return false;
    }

    if (m_hostLevel && m_hostLevel.data() == level) {
        m_hostLevel = nullptr;
        return true; // abrir la ventana para invitar
    }
    return false;
}

void Session::beforeEditorExit(LevelEditorLayer* editor) {
    if (!m_active || editor != m_editor || m_waitingSnapshot || m_snapshotToApply) return;
    if (this->isPlaytesting()) return;
    // lo último que cambiaste se manda ya, sin esperar
    this->scan();
    m_settingsTimer = SETTINGS_INTERVAL;
    this->checkSettings(0.f);
    if (!m_outOps.empty() && Net::get().isReady()) {
        auto body = matjson::Value::object();
        body["ops"] = matjson::Value(m_outOps);
        Net::get().call("/room/ops", std::move(body), nullptr);
        m_outOps.clear();
    }
    this->saveCache(true);
}

// ------------------------------------------------------------------ salir

// Saliste del editor pero sigues en la sala.
void Session::detach(bool notifyUser) {
    this->clearMemberNodes();
    if (!m_outOps.empty() && Net::get().isReady()) {
        auto body = matjson::Value::object();
        body["ops"] = matjson::Value(m_outOps);
        Net::get().call("/room/ops", std::move(body), nullptr);
    }
    this->forgetAllObjects();
    m_editor = nullptr;
    m_sending = false;
    m_sendRetry = -1.f;
    m_snapshotToApply.reset();
    m_job.reset();
    m_waitingSnapshot = false;
    m_selSig.clear();
    m_selPending.reset();
    this->saveCache(true);
    m_needResync = false;
    m_earlyEvents.clear();
    m_playerSentOn = false;
    m_exitTimer = 0.f;
    if (notifyUser) {
        notify("Saliste del editor. Sigues en la sala: abre el nivel para volver.");
    }
}

void Session::leave(bool notifyServer, std::string const& reason) {
    if (!m_active && !m_busy && !m_joining) return;
    log::info("Collab: saliendo de la sala ({})", reason.empty() ? "sin razón" : reason);
    if (notifyServer && Net::get().isReady()) {
        Net::get().call("/room/leave", matjson::Value::object(), nullptr);
    }
    this->clearMemberNodes();
    m_members.clear();
    this->forgetAllObjects();
    m_active = false;
    m_busy = false;
    m_roomId.clear();
    m_ownerId.clear();
    m_levelName.clear();
    m_editor = nullptr;
    m_roomLevel = nullptr;
    m_sending = false;
    m_sendRetry = -1.f;
    m_sendFails = 0;
    m_needResync = false;
    m_waitingSnapshot = false;
    m_cursorInFlight = false;
    m_playerInFlight = false;
    m_playerSentOn = false;
    m_snapshotToApply.reset();
    m_joining = false;
    m_joinSnapshot.reset();
    m_joinLevel = nullptr;
    m_earlyEvents.clear();
    m_lastHeader.clear();
    m_lastMeta.clear();
    m_sentState.clear();
    m_exitTimer = 0.f;
    m_job.reset();
    m_chat.clear();
    m_unread = 0;
    m_viewer = false;
    m_selSig.clear();
    m_selPending.reset();
    m_reviving = false;
    m_reviveRetry = -1.f;
    saveRoomInfo("", "");
    this->clearCache();
}

// ------------------------------------------------------------------ eventos

void Session::onEvent(matjson::Value const& ev) {
    auto type = jstr(ev, "type");

    if (type == "invite") {
        Invite inv;
        inv.roomId = jstr(ev, "roomId");
        inv.levelName = jstr(ev, "levelName");
        inv.fromName = ev.contains("from") ? jstr(ev["from"], "username") : "Alguien";
        if (inv.roomId == m_roomId && m_active) return;
        log::info("Collab: invitación de {} a la sala {}", inv.fromName, inv.roomId);
        bool known = std::any_of(m_inbox.begin(), m_inbox.end(),
            [&](Invite const& i) { return i.roomId == inv.roomId; });
        if (!known) m_inbox.push_back(inv);
        // aunque ya estuviera guardada, se vuelve a mostrar la ventanita
        bool queued = std::any_of(m_alertQueue.begin(), m_alertQueue.end(),
            [&](Invite const& i) { return i.roomId == inv.roomId; });
        bool showing = m_alert && m_alert->roomId == inv.roomId;
        if (!queued && !showing) m_alertQueue.push_back(inv);
        if (showing) m_alertLeft = ALERT_SECONDS;
        m_notifiedInvite.clear();
        return;
    }

    if (type == "revived" && !m_active && !m_joining) {
        // otro de la sala la revivió: vuelves a ella ("fuera del editor")
        auto roomId = jstr(ev, "roomId");
        this->becomeAway(roomId, jstr(ev, "levelName"), "", {});
        auto body = matjson::Value::object();
        Net::get().call("/room/info", std::move(body), [this, roomId](JsonResult res) {
            if (!res || !m_active || m_roomId != roomId) return;
            auto data = res.unwrap();
            if (!data.contains("room") || !data["room"].isObject()) return;
            m_ownerId = jstr(data["room"], "owner");
            this->setMembers(jarr(data["room"], "members"));
        });
        return;
    }
    if (type == "kicked") {
        if (m_active && jstr(ev, "roomId") == m_roomId) this->handleKicked(jstr(ev, "by"));
        return;
    }

    if (m_joining && !m_active) {
        m_earlyEvents.push_back(ev);
        return;
    }
    if (!m_active) return;
    if (m_waitingSnapshot || m_snapshotToApply || m_job) {
        m_earlyEvents.push_back(ev);
        return;
    }

    if (type == "ops") {
        auto from = jstr(ev, "from");
        auto ops = jarr(ev, "ops");
        this->cacheApplyOps(ops);
        if (from == Net::get().clientId()) {
            // eco de lo que enviamos: ya no está pendiente
            for (auto const& op : ops) {
                auto id = jstr(op, "id");
                auto it = m_pending.find(id);
                if (it != m_pending.end() && --it->second <= 0) m_pending.erase(it);
            }
            return;
        }
        if (!this->editor()) return; // fuera del editor: al volver se baja todo
        if (this->isPlaytesting() || !m_deferred.empty()) {
            m_deferred.push_back(std::move(ops));
        } else {
            auto mit = m_members.find(from);
            this->applyRemoteOps(ops, mit != m_members.end() ? mit->second.username : "");
        }
    }
    else if (type == "chat") {
        ChatMsg msg;
        msg.from = jstr(ev, "from");
        msg.username = jstr(ev, "username");
        msg.text = jstr(ev, "text");
        auto mit = m_members.find(msg.from);
        if (mit != m_members.end()) msg.color = mit->second.color;
        m_chat.push_back(msg);
        if (m_chat.size() > 80) m_chat.pop_front();
        if (!m_chatOpen && msg.from != Net::get().clientId()) {
            m_unread++;
            notify(fmt::format("{}: {}", msg.username, msg.text));
        }
    }
    else if (type == "ping") {
        ChatMsg msg;
        msg.from = jstr(ev, "from");
        msg.username = jstr(ev, "username");
        msg.ping = true;
        msg.pos = ccp(jnum(ev, "x"), jnum(ev, "y"));
        msg.text = "marcó un lugar";
        auto mit = m_members.find(msg.from);
        if (mit != m_members.end()) msg.color = mit->second.color;
        m_chat.push_back(msg);
        if (m_chat.size() > 80) m_chat.pop_front();
        this->addMarker(msg.pos, msg.username, msg.color);
        if (msg.from != Net::get().clientId()) {
            if (!m_chatOpen) m_unread++;
            notify(fmt::format("{} marcó un lugar (en el chat puedes ir)", msg.username));
        }
    }
    else if (type == "select") {
        auto mit = m_members.find(jstr(ev, "from"));
        if (mit != m_members.end()) {
            mit->second.selected.clear();
            for (auto const& id : jarr(ev, "ids")) {
                if (id.isString()) mit->second.selected.push_back(id.asString().unwrap());
            }
        }
    }
    else if (type == "owner") {
        auto owner = jstr(ev, "owner");
        m_ownerId = owner;
        if (owner == Net::get().clientId()) {
            m_viewer = false;
            notify("Ahora eres el anfitrión de la sala", NotificationIcon::Success);
        } else if (ev.contains("member")) {
            notify(fmt::format("{} ahora es el anfitrión", jstr(ev["member"], "username")));
        }
    }
    else if (type == "role") {
        auto id = jstr(ev, "client");
        bool viewer = ev.contains("viewer") && ev["viewer"].asBool().unwrapOr(false);
        auto mit = m_members.find(id);
        if (mit != m_members.end()) mit->second.viewer = viewer;
        if (id == Net::get().clientId()) {
            m_viewer = viewer;
            notify(viewer ? "El anfitrión te puso en modo solo mirar" : "El anfitrión te dejó editar otra vez",
                viewer ? NotificationIcon::Warning : NotificationIcon::Success);
        }
        if (mit != m_members.end()) {
            if (viewer) m_cacheViewers.push_back(mit->second.accountID);
            else std::erase(m_cacheViewers, mit->second.accountID);
        }
    }
    else if (type == "cursor") {
        auto it = m_members.find(jstr(ev, "from"));
        if (it != m_members.end()) {
            it->second.target = ccp(jnum(ev, "x"), jnum(ev, "y"));
            if (!it->second.hasCursor && it->second.cursor) {
                it->second.cursor->setPosition(it->second.target);
            }
            it->second.hasCursor = true;
        }
    }
    else if (type == "player") {
        this->onPlayerEvent(ev);
    }
    else if (type == "status") {
        auto it = m_members.find(jstr(ev, "from"));
        if (it != m_members.end()) {
            it->second.state = jstr(ev, "state", "edit");
            if (it->second.state == "away") it->second.hasCursor = false;
        }
    }
    else if (type == "join") {
        if (ev.contains("member")) {
            auto const& m = ev["member"];
            if (!m_members.contains(jstr(m, "client"))) {
                this->addMember(m);
                notify(fmt::format("{} entró a la sala", jstr(m, "username")));
            }
        }
    }
    else if (type == "leave") {
        if (ev.contains("member")) {
            auto const& m = ev["member"];
            auto id = jstr(m, "client");
            if (id != Net::get().clientId()) {
                this->removeMember(id);
                bool dropped = ev.contains("disconnected") && ev["disconnected"].asBool().unwrapOr(false);
                auto name = jstr(m, "username");
                notify(dropped ? fmt::format("{} se desconectó (volverá a la sala al abrir el juego)", name)
                               : fmt::format("{} salió de la sala", name));
            }
        }
        auto owner = jstr(ev, "owner");
        if (!owner.empty() && owner != m_ownerId) {
            m_ownerId = owner;
            if (owner == Net::get().clientId()) {
                notify("Ahora eres el anfitrión de la sala", NotificationIcon::Info);
            }
        }
    }
    else if (type == "resync") {
        if (this->editor()) this->requestSnapshot(false);
    }
}

// ------------------------------------------------------------------ sincronizar

void Session::requestSnapshot(bool removeUntracked) {
    m_needResync = true;
    m_resyncRemoveUntracked = m_resyncRemoveUntracked || removeUntracked;
    m_waitingSnapshot = true;
    m_resyncDelay = 0.f;
}

void Session::queueOp(char type, std::string const& id, std::string const* str) {
    auto op = matjson::Value::object();
    op["t"] = std::string(1, type);
    op["id"] = id;
    if (str) op["s"] = *str;
    m_outOps.push_back(std::move(op));
    m_pending[id] += 1;
}

void Session::scan() {
    auto ed = this->editor();
    if (!ed || !ed->m_objects) return;
    m_scanTick++;

    // lo seleccionado se revisa siempre (es lo que más cambia, incluye triggers abiertos)
    std::unordered_set<GameObject*> selected;
    if (auto ui = ed->m_editorUI) {
        if (ui->m_selectedObject) selected.insert(ui->m_selectedObject);
        if (ui->m_selectedObjects) {
            for (auto obj : CCArrayExt<GameObject*>(ui->m_selectedObjects)) selected.insert(obj);
        }
    }
    this->updateSelection(selected);
    if (m_viewer) {
        this->viewerCheck(selected);
        return;
    }

    size_t count = ed->m_objects->count();
    bool fullScan = count <= SCAN_CHUNK;
    size_t chunkStart = fullScan ? 0 : (m_scanCursor % count);
    size_t chunkEnd = chunkStart + SCAN_CHUNK; // puede pasar de count (se da la vuelta)

    size_t index = 0;
    for (auto obj : CCArrayExt<GameObject*>(ed->m_objects)) {
        auto it = m_byPtr.find(obj);
        if (it == m_byPtr.end()) {
            // objeto nuevo puesto por ti
            auto id = this->newObjectId();
            auto str = this->saveString(obj);
            this->registerObject(id, obj, str);
            this->queueOp('a', id, &str);
        }
        else {
            it->second.seen = m_scanTick;
            bool inChunk = fullScan
                || (index >= chunkStart && index < chunkEnd)
                || (chunkEnd > count && index < chunkEnd - count);
            bool checkSelected = selected.size() <= SELECTED_FULL_LIMIT || (m_scanTick % 4) == 0;
            if (inChunk || (checkSelected && selected.contains(obj))) {
                auto str = this->saveString(obj);
                if (str != it->second.str) {
                    it->second.str = str;
                    this->queueOp('m', it->second.id, &str);
                }
            }
        }
        index++;
    }
    if (!fullScan && count > 0) m_scanCursor = chunkEnd % count;

    // los que ya no están en el nivel fueron borrados
    std::vector<GameObject*> gone;
    for (auto& [ptr, info] : m_byPtr) {
        if (info.seen != m_scanTick) gone.push_back(ptr);
    }
    for (auto ptr : gone) {
        auto it = m_byPtr.find(ptr);
        auto id = it->second.id;
        m_byPtr.erase(it);
        this->queueOp('d', id, nullptr);
        auto bit = m_byId.find(id);
        if (bit != m_byId.end()) {
            m_graveyard.push_back(bit->second);
            m_byId.erase(bit);
        }
    }
}

void Session::flushOps() {
    if (m_sending || m_outOps.empty() || m_sendRetry >= 0.f) return;
    if (!Net::get().isReady()) return;

    std::vector<matjson::Value> batch;
    if (m_outOps.size() <= MAX_OPS_PER_SEND) {
        batch = std::move(m_outOps);
        m_outOps.clear();
    } else {
        batch.assign(
            std::make_move_iterator(m_outOps.begin()),
            std::make_move_iterator(m_outOps.begin() + MAX_OPS_PER_SEND)
        );
        m_outOps.erase(m_outOps.begin(), m_outOps.begin() + MAX_OPS_PER_SEND);
    }

    auto body = matjson::Value::object();
    body["ops"] = matjson::Value(batch);
    m_sending = true;
    auto room = m_roomId;
    auto ed = m_editor;

    Net::get().call("/room/ops", std::move(body), [this, room, ed, batch](JsonResult res) mutable {
        if (!m_active || room != m_roomId || ed != m_editor) return;
        m_sending = false;
        if (res) {
            m_sendFails = 0;
            this->flushOps();
            return;
        }
        if (res.unwrapErr().find("solo mirar") != std::string::npos) {
            // no puedes editar: se deshacen tus cambios
            m_viewer = true;
            m_pending.clear();
            m_outOps.clear();
            this->requestSnapshot(true);
            return;
        }
        // se vuelve a intentar con los mismos cambios al principio de la fila
        m_outOps.insert(m_outOps.begin(),
            std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
        m_sendFails++;
        log::warn("Collab: no se pudieron enviar cambios: {}", res.unwrapErr());
        if (m_sendFails >= 12) {
            // no se sale de la sala: se vuelve a pedir la copia cuando haya conexión
            notify("Problemas de conexión. Reintentando...", NotificationIcon::Warning);
            m_sendFails = 0;
        }
        m_sendRetry = std::min(1.f * (m_sendFails + 1), 5.f);
    });
}

GameObject* Session::createLocal(std::string const& id, std::string const& str) {
    auto ed = this->editor();
    if (!ed || str.empty()) return nullptr;
    auto arr = ed->createObjectsFromString(str, true, true);
    if (!arr || arr->count() == 0) return nullptr;
    auto obj = static_cast<GameObject*>(arr->objectAtIndex(0));
    this->registerObject(id, obj, this->saveString(obj));
    return obj;
}

static bool isSelectedLocally(LevelEditorLayer* ed, GameObject* obj) {
    if (!ed || !obj) return false;
    auto ui = ed->m_editorUI;
    if (!ui) return false;
    return ui->m_selectedObject == obj
        || (ui->m_selectedObjects && ui->m_selectedObjects->containsObject(obj));
}

void Session::removeLocal(GameObject* obj) {
    auto ed = this->editor();
    Ref<GameObject> keep = obj;
    if (ed) {
        if (auto ui = ed->m_editorUI) {
            if (isSelectedLocally(ed, obj)) ui->deselectObject(obj);
        }
        ed->removeObject(obj, true);
    }
    // se guarda un rato por si alguna ventana todavía lo usa
    m_graveyard.push_back(keep);
    m_removedForUndo.insert(obj);
    auto it = m_byPtr.find(obj);
    if (it != m_byPtr.end()) {
        auto id = it->second.id;
        m_byPtr.erase(it);
        m_byId.erase(id);
    }
}

void Session::applyRemoteOps(std::vector<matjson::Value> const& ops, std::string const& fromName) {
    auto ed = this->editor();
    if (!ed) return;
    bool changed = false;

    for (auto const& op : ops) {
        auto id = jstr(op, "id");
        auto t = jstr(op, "t");
        if (id.empty()) continue;

        if (t == "h") {
            this->applyHeader(jstr(op, "s"));
            continue;
        }
        if (t == "g") {
            this->applyMeta(jstr(op, "s"));
            continue;
        }
        // si tú cambiaste este objeto y el servidor aún no lo recibe, gana tu cambio
        if (m_pending.contains(id)) continue;

        auto it = m_byId.find(id);
        GameObject* existing = it != m_byId.end() ? it->second.data() : nullptr;

        // lo tienes seleccionado (o con su ventana de ajustes abierta): se aplica al soltarlo
        if (existing && isSelectedLocally(ed, existing)) {
            m_held[id] = op;
            if (m_conflictCooldown <= 0.f && !fromName.empty()) {
                m_conflictCooldown = 6.f;
                notify(fmt::format("{} también está cambiando lo que tienes seleccionado", fromName),
                    NotificationIcon::Warning);
            }
            continue;
        }
        m_held.erase(id);

        if (t == "a" || t == "m") {
            auto s = jstr(op, "s");
            if (existing) {
                auto pit = m_byPtr.find(existing);
                if (pit != m_byPtr.end() && pit->second.str == s) continue;
                this->removeLocal(existing);
            }
            this->createLocal(id, s);
            changed = true;
        }
        else if (t == "d") {
            if (existing) {
                this->removeLocal(existing);
                changed = true;
            }
        }
    }

    if (changed) {
        this->purgeUndo();
        if (auto ui = ed->m_editorUI) {
            ui->updateButtons();
            ui->updateObjectInfoLabel();
        }
    }
}

// ------------------------------------------------------------------ ajustes del nivel, fondo y canción

std::string Session::settingsString(LevelEditorLayer* ed) {
    if (!ed || !ed->m_levelSettings) return "";
    return ed->m_levelSettings->getSaveString();
}

// La canción y los efectos de sonido no van en los ajustes, van en el nivel.
std::string Session::metaString(LevelEditorLayer* ed) {
    if (!ed || !ed->m_level) return "";
    auto level = ed->m_level;
    auto v = matjson::Value::object();
    v["audioTrack"] = level->m_audioTrack;
    v["songID"] = level->m_songID;
    v["songIDs"] = std::string(level->m_songIDs);
    v["sfxIDs"] = std::string(level->m_sfxIDs);
    return v.dump(matjson::NO_INDENTATION);
}

static bool syncSettingsEnabled() {
    return Mod::get()->getSettingValue<bool>("sync-settings");
}

// Cada poquito: si cambiaste colores, fondo, canción, velocidad, etc., se envían.
void Session::checkSettings(float dt) {
    m_settingsTimer += dt;
    if (m_settingsTimer < SETTINGS_INTERVAL) return;
    m_settingsTimer = 0.f;
    auto ed = this->editor();
    if (!ed) return;

    if (m_viewer) return;
    auto meta = this->metaString(ed);
    if (!meta.empty() && meta != m_lastMeta) {
        m_lastMeta = meta;
        this->queueOp('g', "__meta__", &meta);
        log::info("Collab: enviando canción del nivel: {}", meta);
    }

    if (!syncSettingsEnabled()) return;
    auto now = this->settingsString(ed);
    if (now.empty() || now == m_lastHeader) return;
    m_lastHeader = now;
    this->queueOp('h', "__header__", &now);
    // tu propio fondo ya está bien, solo se recuerda
    if (auto s = ed->m_levelSettings) {
        m_bgIndex = s->m_backgroundIndex;
        m_mgIndex = s->m_middleGroundIndex;
        m_groundIndex = s->m_groundIndex;
        m_lineIndex = s->m_groundLineIndex;
    }
}

void Session::applyHeader(std::string const& header) {
    if (!syncSettingsEnabled() || header.empty()) return;
    if (m_pending.contains("__header__")) return; // tu cambio va después
    auto ed = this->editor();
    if (!ed) return;
    if (header == this->settingsString(ed)) {
        m_lastHeader = header;
        return;
    }
    // GD lee los ajustes desde la primera parte del texto del nivel.
    // La canción no va ahí: se guarda antes y se repone después, por si GD la cambia.
    auto metaBefore = this->metaString(ed);
    std::string setup = header + ";";
    ed->createObjectsFromSetup(setup);
    ed->levelSettingsUpdated();
    if (!metaBefore.empty() && this->metaString(ed) != metaBefore) this->restoreMeta(ed, metaBefore);

    // fondo, piso y fondo del medio: se vuelven a crear si cambiaron
    if (auto s = ed->m_levelSettings) {
        if (s->m_backgroundIndex != m_bgIndex) {
            ed->createBackground(s->m_backgroundIndex);
        }
        if (s->m_middleGroundIndex != m_mgIndex) {
            ed->createMiddleground(s->m_middleGroundIndex);
        }
        if (s->m_groundIndex != m_groundIndex || s->m_groundLineIndex != m_lineIndex) {
            ed->createGroundLayer(s->m_groundIndex, s->m_groundLineIndex);
        }
        m_bgIndex = s->m_backgroundIndex;
        m_mgIndex = s->m_middleGroundIndex;
        m_groundIndex = s->m_groundIndex;
        m_lineIndex = s->m_groundLineIndex;
    }
    ed->updateLevelColors();
    m_lastHeader = this->settingsString(ed);
    m_lastMeta = this->metaString(ed);
    log::info("Collab: ajustes del nivel actualizados");
}

void Session::applyMeta(std::string const& meta) {
    if (meta.empty() || m_pending.contains("__meta__")) return;
    auto ed = this->editor();
    if (!ed || !ed->m_level) return;
    if (meta == this->metaString(ed)) {
        m_lastMeta = meta;
        return;
    }
    this->restoreMeta(ed, meta);
    m_metaRemoteAt = m_clock;
    log::info("Collab: canción del nivel actualizada: {}", meta);
    notify("Cambiaron la canción del nivel");
}

// Pone la canción y los efectos de sonido tal cual vienen en el texto.
void Session::restoreMeta(LevelEditorLayer* ed, std::string const& meta) {
    if (!ed || !ed->m_level) return;
    auto parsed = matjson::parse(meta);
    if (!parsed) return;
    auto v = parsed.unwrap();
    auto level = ed->m_level;
    level->m_audioTrack = (int)jnum(v, "audioTrack", level->m_audioTrack);
    level->m_songID = (int)jnum(v, "songID", level->m_songID);
    level->m_songIDs = jstr(v, "songIDs", std::string(level->m_songIDs));
    level->m_sfxIDs = jstr(v, "sfxIDs", std::string(level->m_sfxIDs));
    if (ed->m_levelSettings && ed->m_levelSettings->m_level && ed->m_levelSettings->m_level != level) {
        auto other = ed->m_levelSettings->m_level;
        other->m_audioTrack = level->m_audioTrack;
        other->m_songID = level->m_songID;
        other->m_songIDs = level->m_songIDs;
        other->m_sfxIDs = level->m_sfxIDs;
    }
    m_lastMeta = this->metaString(ed);
}

std::string Session::metaNow(LevelEditorLayer* ed) {
    return this->metaString(ed);
}

// La ventana de ajustes del nivel se cerró. Si la tenías abierta mientras otro
// cambiaba la canción y tú no la tocaste, se repone la canción nueva (si no,
// la ventana escribiría la vieja encima).
void Session::settingsLayerClosed(LevelEditorLayer* ed, std::string const& metaBefore, bool songTouched, float openedAt) {
    if (!m_active || ed != m_editor || !this->editor()) return;
    auto now = this->metaString(ed);
    bool remoteWhileOpen = m_metaRemoteAt >= openedAt;
    if (!songTouched && remoteWhileOpen && now != metaBefore && !m_lastMeta.empty()) {
        this->restoreMeta(ed, m_lastMeta);
    }
    // se revisa ya mismo, sin esperar
    m_settingsTimer = SETTINGS_INTERVAL;
    this->checkSettings(0.f);
}

// ------------------------------------------------------------------ miembros, cursores y cubos

ccColor3B Session::nextColor() {
    static const ccColor3B palette[] = {
        {0, 255, 140}, {255, 90, 90}, {80, 170, 255},
        {255, 210, 60}, {200, 110, 255}, {255, 150, 40},
    };
    auto c = palette[m_colorIndex % 6];
    m_colorIndex++;
    return c;
}

static CCNode* makeCursorNode(std::string const& name, ccColor3B color) {
    auto node = CCNode::create();

    auto draw = CCDrawNode::create();
    CCPoint arrow[] = {
        ccp(0, 0), ccp(0, -22), ccp(6, -16), ccp(10, -25),
        ccp(14, -23), ccp(10, -14), ccp(17, -14),
    };
    ccColor4F fill = ccc4FFromccc3B(color);
    draw->drawPolygon(arrow, 7, fill, 1.5f, ccc4f(0, 0, 0, 1));
    node->addChild(draw);

    auto label = CCLabelBMFont::create(name.c_str(), "chatFont.fnt");
    label->setAnchorPoint(ccp(0, 1));
    label->setPosition(ccp(14, -24));
    label->setScale(0.7f);
    label->setColor(color);
    node->addChild(label);

    node->setZOrder(100000);
    node->setVisible(false);
    return node;
}

void Session::addMember(matjson::Value const& m) {
    Member member;
    member.clientId = jstr(m, "client");
    member.accountID = (int)jnum(m, "accountID");
    member.username = jstr(m, "username");
    member.state = jstr(m, "state", "edit");
    member.viewer = m.contains("viewer") && m["viewer"].asBool().unwrapOr(false);
    if (member.clientId.empty() || m_members.contains(member.clientId)) return;
    if (member.clientId == Net::get().clientId()) m_viewer = member.viewer;
    member.color = this->nextColor();
    if (member.clientId != Net::get().clientId()) {
        member.cursor = makeCursorNode(member.username, member.color);
    }
    m_members[member.clientId] = std::move(member);
}

void Session::removeMember(std::string const& clientId) {
    auto it = m_members.find(clientId);
    if (it == m_members.end()) return;
    if (it->second.cursor) it->second.cursor->removeFromParent();
    if (it->second.ghost) it->second.ghost->removeFromParent();
    if (it->second.selDraw) it->second.selDraw->removeFromParent();
    m_members.erase(it);
}

void Session::setMembers(std::vector<matjson::Value> const& list) {
    this->clearMemberNodes();
    m_members.clear();
    m_colorIndex = 0;
    for (auto const& m : list) this->addMember(m);
}

void Session::updateCursorNode(Member& m, float dt) {
    auto ed = this->editor();
    if (!ed || !m.cursor || !ed->m_objectLayer) return;
    if (m.cursor->getParent() != ed->m_objectLayer) {
        m.cursor->removeFromParent();
        ed->m_objectLayer->addChild(m.cursor, 100000);
    }
    m.cursor->setVisible(m.hasCursor && m.state == "edit");
    auto pos = m.cursor->getPosition();
    float k = std::min(1.f, dt * 15.f);
    m.cursor->setPosition(pos + (m.target - pos) * k);
    float scale = ed->m_objectLayer->getScale();
    if (scale > 0.0001f) m.cursor->setScale(1.f / scale);
}

static IconType modeToIcon(int mode) {
    switch (mode) {
        case 1: return IconType::Ship;
        case 2: return IconType::Ball;
        case 3: return IconType::Ufo;
        case 4: return IconType::Wave;
        case 5: return IconType::Robot;
        case 6: return IconType::Spider;
        case 7: return IconType::Swing;
        default: return IconType::Cube;
    }
}

static ccColor3B intToColor(int c) {
    return { (GLubyte)((c >> 16) & 255), (GLubyte)((c >> 8) & 255), (GLubyte)(c & 255) };
}

void Session::onPlayerEvent(matjson::Value const& ev) {
    auto it = m_members.find(jstr(ev, "from"));
    if (it == m_members.end() || it->first == Net::get().clientId()) return;
    auto& m = it->second;
    bool on = ev.contains("on") && ev["on"].asBool().unwrapOr(false);
    m.ghostOn = on;
    m.ghostAge = 0.f;
    if (!on) return;

    int mode = (int)jnum(ev, "m");
    int icon = std::max(1, (int)jnum(ev, "i", 1));
    auto target = ccp(jnum(ev, "x"), jnum(ev, "y"));

    if (!m.ghost) {
        auto node = CCNode::create();
        auto player = SimplePlayer::create(1);
        player->setOpacity(190);
        node->addChild(player);
        auto label = CCLabelBMFont::create(m.username.c_str(), "chatFont.fnt");
        label->setScale(0.55f);
        label->setColor(m.color);
        label->setPosition({0, 26});
        label->setID("name");
        node->addChild(label);
        node->setZOrder(99999);
        m.ghost = node;
        m.ghostIcon = player;
        m.ghostMode = -1;
        m.ghost->setPosition(target);
    }
    if (mode != m.ghostMode || icon != m.ghostIconId) {
        m.ghostIcon->updatePlayerFrame(icon, modeToIcon(mode));
        m.ghostMode = mode;
        m.ghostIconId = icon;
    }
    m.ghostIcon->setColors(intToColor((int)jnum(ev, "c1")), intToColor((int)jnum(ev, "c2")));
    m.ghostTarget = target;
    m.ghostRot = (float)jnum(ev, "r");
    m.ghostScale = (float)jnum(ev, "s", 1);
    m.ghostFlip = ev.contains("f") && ev["f"].asBool().unwrapOr(false);
}

void Session::updateGhostNode(Member& m, float dt) {
    if (!m.ghost) return;
    auto ed = this->editor();
    if (!ed || !ed->m_objectLayer) {
        m.ghost->setVisible(false);
        return;
    }
    if (m.ghost->getParent() != ed->m_objectLayer) {
        m.ghost->removeFromParent();
        ed->m_objectLayer->addChild(m.ghost, 99999);
    }
    m.ghostAge += dt;
    bool visible = m.ghostOn && m.ghostAge < 2.f && m.state == "test";
    m.ghost->setVisible(visible);
    if (!visible) return;
    auto pos = m.ghost->getPosition();
    float k = std::min(1.f, dt * 18.f);
    m.ghost->setPosition(pos + (m.ghostTarget - pos) * k);
    if (m.ghostIcon) {
        m.ghostIcon->setRotation(m.ghostRot);
        m.ghostIcon->setScaleX(m.ghostScale);
        m.ghostIcon->setScaleY(m.ghostFlip ? -m.ghostScale : m.ghostScale);
    }
}

void Session::sendCursor() {
    auto ed = this->editor();
    if (!ed || !ed->m_objectLayer || m_cursorInFlight || !Net::get().isReady()) return;
    if (m_members.size() <= 1 || this->isPlaytesting()) return; // nadie más lo va a ver
#ifdef GEODE_IS_ANDROID
    // en el celular no hay mouse: se usa el centro de la pantalla
    auto screen = CCDirector::get()->getWinSize() / 2.f;
#else
    auto screen = geode::cocos::getMousePos();
#endif
    auto pos = ed->m_objectLayer->convertToNodeSpace(screen);
    if (ccpDistance(pos, m_lastCursorSent) < 2.f) return;
    m_lastCursorSent = pos;
    m_cursorInFlight = true;

    auto body = matjson::Value::object();
    body["x"] = std::round(pos.x * 10.f) / 10.f;
    body["y"] = std::round(pos.y * 10.f) / 10.f;
    Net::get().call("/room/cursor", std::move(body), [this](JsonResult) {
        m_cursorInFlight = false;
    });
}

static int colorToInt(ccColor3B c) {
    return (c.r << 16) | (c.g << 8) | c.b;
}

// Mientras pruebas el nivel, los demás ven tu cubo.
void Session::sendPlayer(float dt) {
    auto ed = this->editor();
    if (!ed || !Net::get().isReady() || m_members.size() <= 1) return;
    bool testing = this->isPlaytesting() && ed->m_player1;

    if (!testing) {
        if (m_playerSentOn && !m_playerInFlight) {
            m_playerSentOn = false;
            auto body = matjson::Value::object();
            body["on"] = false;
            Net::get().call("/room/player", std::move(body), nullptr);
        }
        return;
    }

    m_playerTimer += dt;
    if (m_playerTimer < PLAYER_INTERVAL || m_playerInFlight) return;
    m_playerTimer = 0.f;

    auto p = ed->m_player1;
    int mode = 0;
    if (p->m_isShip) mode = 1;
    else if (p->m_isBall) mode = 2;
    else if (p->m_isBird) mode = 3;
    else if (p->m_isDart) mode = 4;
    else if (p->m_isRobot) mode = 5;
    else if (p->m_isSpider) mode = 6;
    else if (p->m_isSwing) mode = 7;

    auto gm = GameManager::get();
    auto pos = p->getPosition();
    auto body = matjson::Value::object();
    body["on"] = true;
    body["x"] = std::round(pos.x * 10.f) / 10.f;
    body["y"] = std::round(pos.y * 10.f) / 10.f;
    body["r"] = std::round(p->getRotation());
    body["m"] = mode;
    body["i"] = gm->activeIconForType(modeToIcon(mode));
    body["s"] = std::round(p->m_vehicleSize * 100.f) / 100.f;
    body["f"] = p->m_isUpsideDown;
    body["c1"] = colorToInt(gm->colorForIdx(gm->getPlayerColor()));
    body["c2"] = colorToInt(gm->colorForIdx(gm->getPlayerColor2()));
    m_playerInFlight = true;
    m_playerSentOn = true;
    Net::get().call("/room/player", std::move(body), [this](JsonResult) {
        m_playerInFlight = false;
    });
}

std::string Session::myState() const {
    if (!this->editor() && !m_waitingSnapshot && !m_snapshotToApply) return "away";
    return this->isPlaytesting() ? "test" : "edit";
}

void Session::sendStatus() {
    if (m_statusInFlight || !Net::get().isReady()) return;
    auto state = this->myState();
    if (state == m_sentState) return;
    m_statusInFlight = true;
    auto body = matjson::Value::object();
    body["state"] = state;
    Net::get().call("/room/status", std::move(body), [this, state](JsonResult res) {
        m_statusInFlight = false;
        if (res) m_sentState = state;
    });
    // tu propia fila en la lista
    auto me = m_members.find(Net::get().clientId());
    if (me != m_members.end()) me->second.state = state;
}

// ------------------------------------------------------------------ invitaciones

void Session::removeInvite(std::string const& roomId, bool tellServer) {
    if (tellServer && Net::get().isReady()) {
        auto body = matjson::Value::object();
        body["roomId"] = roomId;
        Net::get().call("/invite/dismiss", std::move(body), nullptr);
    }
    std::erase_if(m_inbox, [&](Invite const& i) { return i.roomId == roomId; });
    std::erase_if(m_alertQueue, [&](Invite const& i) { return i.roomId == roomId; });
    if (m_alert && m_alert->roomId == roomId) {
        if (auto alert = typeinfo_cast<InviteAlert*>(m_alertNode.data())) alert->closeSilently();
        m_alert.reset();
        m_alertNode = nullptr;
    }
}

void Session::alertClosed(std::string const& roomId, bool accepted) {
    if (!m_alert || m_alert->roomId != roomId) return;
    auto inv = *m_alert;
    m_alert.reset();
    m_alertNode = nullptr;
    if (accepted) {
        this->accept(inv);
    } else {
        notify("La invitación quedó guardada en tu perfil (botón Collab).");
    }
}

void Session::updateInviteAlert(float dt) {
    if (!m_alert) {
        while (!m_alertQueue.empty()) {
            auto inv = m_alertQueue.front();
            m_alertQueue.pop_front();
            bool stillThere = std::any_of(m_inbox.begin(), m_inbox.end(),
                [&](Invite const& i) { return i.roomId == inv.roomId; });
            if (stillThere) {
                m_alert = inv;
                m_alertLeft = ALERT_SECONDS;
                break;
            }
        }
        if (!m_alert) return;
    }
    if (inTransition()) return;

    if (playingLevel()) {
        // jugando: solo un aviso, la ventana sale cuando salgas del nivel
        if (m_notifiedInvite != m_alert->roomId) {
            m_notifiedInvite = m_alert->roomId;
            notify(fmt::format("{} te invitó a Collab. Sal del nivel para responder.", m_alert->fromName));
        }
        return;
    }

    // si cambiaste de pantalla, la ventanita se vuelve a abrir en la nueva
    if (!m_alertNode || !inRunningScene(m_alertNode)) {
        if (m_alertNode) {
            if (auto old = typeinfo_cast<InviteAlert*>(m_alertNode.data())) old->closeSilently();
            m_alertNode = nullptr;
        }
        if (auto alert = InviteAlert::create(*m_alert, currentEditor() != nullptr)) {
            alert->show();
            m_alertNode = alert;
        }
        return;
    }

    m_alertLeft -= dt;
    if (m_alertLeft <= 0.f) {
        if (auto alert = typeinfo_cast<InviteAlert*>(m_alertNode.data())) alert->closeSilently();
        m_alertNode = nullptr;
        m_alert.reset();
        notify("La invitación quedó guardada en tu perfil (botón Collab).");
    }
}

// ------------------------------------------------------------------ cada frame

void Session::tick(float dt) {
    if (!m_initialized) return;
    m_clock += dt;
    this->updateInviteAlert(dt);

    // los objetos borrados se sueltan después de un rato
    if (m_graveyard.size() > 4000) m_graveyard.erase(m_graveyard.begin(), m_graveyard.begin() + 2000);
    m_graveyardTimer += dt;
    if (m_graveyardTimer > 20.f) {
        m_graveyardTimer = 0.f;
        if (!this->editor() || m_graveyard.size() > 500) m_graveyard.clear();
    }

    // invitación aceptada estando en un nivel: entrar apenas salga
    if (m_pendingJoin && !m_active && !m_busy && !m_joining && Net::get().isReady()
        && !inTransition() && !currentEditor() && !playingLevel()) {
        auto inv = *m_pendingJoin;
        m_pendingJoin.reset();
        this->join(inv);
    }

    if (m_joining && !m_active) {
        m_graceTimer += dt;
        if (m_graceTimer > 30.f) {
            this->leave(true, "no se abrió el nivel de la sala");
            notify("No se pudo abrir el nivel de la sala", NotificationIcon::Error);
        }
        return;
    }
    if (!m_active) return;

    this->sendStatus();
    if (m_conflictCooldown > 0.f) m_conflictCooldown -= dt;
    if (m_viewerCooldown > 0.f) m_viewerCooldown -= dt;

    // copia local: se guarda en el disco cada 15 s si cambió
    m_cacheTimer += dt;
    if (m_cacheTimer > 30.f) {
        m_cacheTimer = 0.f;
        this->saveCache(false);
    }
    if (m_reviveRetry >= 0.f) {
        m_reviveRetry -= dt;
        if (m_reviveRetry < 0.f && Net::get().isReady()) this->tryRevive();
    }

    // pedir la copia de la sala (al volver al editor o después de un corte)
    if (m_needResync && !m_busy && Net::get().isReady()) {
        m_resyncDelay -= dt;
        if (m_resyncDelay <= 0.f) {
            m_needResync = false;
            m_busy = true;
            auto body = matjson::Value::object();
            body["roomId"] = m_roomId;
            body["state"] = this->myState();
            auto room = m_roomId;
            Net::get().call("/room/join", std::move(body), [this, room](JsonResult res) {
                m_busy = false;
                if (!m_active || room != m_roomId) return;
                if (!res) {
                    auto err = res.unwrapErr();
                    if (err.find("no existe") != std::string::npos) {
                        // el servidor se reinició: se revive con tu copia
                        this->tryRevive();
                        return;
                    }
                    bool gone = err.find("invitación") != std::string::npos
                        || err.find("otra sala") != std::string::npos;
                    if (gone) {
                        FLAlertLayer::create("Collab",
                            "Se cerró la sala: " + err + "\nTus cambios siguen en tu nivel.", "OK")->show();
                        this->leave(false, "se perdió la sala");
                        return;
                    }
                    // sin internet o el servidor despertando: se reintenta (nunca te saca por esto)
                    m_needResync = true;
                    m_resyncDelay = std::min(3.f * (1 + ++m_resyncFails), 20.f);
                    if (m_resyncFails == 3) notify("Problemas de conexión, reintentando...", NotificationIcon::Warning);
                    return;
                }
                m_resyncFails = 0;
                auto snap = res.unwrap();
                this->cacheFromSnapshot(snap);
                m_snapshotToApply = std::move(snap);
            }, 120);
        }
    }

    auto ed = this->editor();
    if (!ed) {
        if (m_editor) {
            // el editor puede tardar en aparecer en la escena; se espera un poco
            // antes de decidir que saliste (así no te saca por un parpadeo)
            m_exitTimer += dt;
            float limit = (m_snapshotToApply || m_waitingSnapshot || m_job) ? 10.f : 1.5f;
            if (m_exitTimer > limit) this->detach(true);
        }
        return;
    }
    m_exitTimer = 0.f;

    if (m_snapshotToApply && !m_job) {
        auto snap = std::move(*m_snapshotToApply);
        m_snapshotToApply.reset();
        this->startSnapshot(std::move(snap), m_resyncRemoveUntracked);
        m_resyncRemoveUntracked = false;
    }
    if (m_job) {
        // niveles grandes se cargan de a poco para que el juego no se congele
        if (!this->stepSnapshot()) return;
        this->finishSnapshot();
    }
    if (m_waitingSnapshot) return;

    if (m_sendRetry >= 0.f) {
        m_sendRetry -= dt;
        if (m_sendRetry < 0.f) m_sendRetry = -1.f;
    }

    if (!this->isPlaytesting() && !m_busy) {
        while (!m_deferred.empty()) {
            auto ops = std::move(m_deferred.front());
            m_deferred.pop_front();
            this->applyRemoteOps(ops);
        }
        // cambios de otros a objetos que tenías seleccionados
        if (!m_held.empty()) {
            std::vector<matjson::Value> ready;
            for (auto it = m_held.begin(); it != m_held.end();) {
                auto bit = m_byId.find(it->first);
                if (bit == m_byId.end() || !isSelectedLocally(ed, bit->second.data())) {
                    ready.push_back(it->second);
                    it = m_held.erase(it);
                } else {
                    ++it;
                }
            }
            if (!ready.empty()) this->applyRemoteOps(ready);
        }
        this->checkSettings(dt);
        m_scanTimer += dt;
        if (m_scanTimer >= SCAN_INTERVAL) {
            m_scanTimer = 0.f;
            this->scan();
        }
        this->flushOps();
    }

    this->sendSelection(dt);
    this->drawSelections(dt);
    this->updateMarkers(dt);

    m_cursorTimer += dt;
    if (m_cursorTimer >= CURSOR_INTERVAL) {
        m_cursorTimer = 0.f;
        this->sendCursor();
    }
    this->sendPlayer(dt);
    for (auto& [id, m] : m_members) {
        this->updateCursorNode(m, dt);
        this->updateGhostNode(m, dt);
    }
}

// ------------------------------------------------------------------ copia local de la sala

// Cada uno guarda en su PC una copia de la sala. Si el servidor se reinicia
// y la sala se pierde, el primero que vuelve la revive con su copia.

static std::filesystem::path cachePath() {
    return Mod::get()->getSaveDir() / "room-cache.json";
}

void Session::cacheTouch() {
    m_cacheSavedAt = nowMs();
    m_cacheDirty = true;
}

void Session::cacheFromSnapshot(matjson::Value const& snap) {
    m_cacheRoom = jstr(snap, "roomId", m_roomId);
    m_cacheLevelName = jstr(snap, "levelName", m_levelName);
    m_cacheHeader = jstr(snap, "header");
    m_cacheMeta = jstr(snap, "meta");
    m_cacheObjs.clear();
    for (auto const& pair : jarr(snap, "objects")) {
        if (!pair.isArray()) continue;
        auto const& arr = pair.asArray().unwrap();
        if (arr.size() < 2) continue;
        m_cacheObjs[arr[0].asString().unwrapOr("")] = arr[1].asString().unwrapOr("");
    }
    m_cacheAllowed.clear();
    for (auto const& a : jarr(snap, "allowed")) m_cacheAllowed.push_back((int)a.asInt().unwrapOr(0));
    m_cacheViewers.clear();
    for (auto const& m : jarr(snap, "members")) {
        if (m.contains("viewer") && m["viewer"].asBool().unwrapOr(false)) {
            m_cacheViewers.push_back((int)jnum(m, "accountID"));
        }
    }
    // la conversación de la sala
    if (snap.contains("chat")) {
        m_chat.clear();
        for (auto const& c : jarr(snap, "chat")) {
            ChatMsg msg;
            msg.from = jstr(c, "from");
            msg.username = jstr(c, "username");
            msg.text = jstr(c, "text");
            m_chat.push_back(msg);
        }
    }
    this->cacheTouch();
}

void Session::cacheApplyOps(std::vector<matjson::Value> const& ops) {
    if (m_cacheRoom != m_roomId) return;
    for (auto const& op : ops) {
        auto t = jstr(op, "t");
        auto id = jstr(op, "id");
        if (t == "a" || t == "m") m_cacheObjs[id] = jstr(op, "s");
        else if (t == "d") m_cacheObjs.erase(id);
        else if (t == "h") m_cacheHeader = jstr(op, "s");
        else if (t == "g") m_cacheMeta = jstr(op, "s");
    }
    this->cacheTouch();
}

static matjson::Value cacheToJson(std::string const& room, std::string const& level, std::string const& header,
    std::string const& meta, std::unordered_map<std::string, std::string> const& objs,
    std::vector<int> const& allowed, std::vector<int> const& viewers, int64_t savedAt) {
    auto v = matjson::Value::object();
    v["roomId"] = room;
    v["levelName"] = level;
    v["header"] = header;
    v["meta"] = meta;
    v["savedAt"] = (double)savedAt;
    auto arr = matjson::Value::array();
    for (auto const& [id, str] : objs) {
        auto pair = matjson::Value::array();
        pair.push(id);
        pair.push(str);
        arr.push(std::move(pair));
    }
    v["objects"] = std::move(arr);
    auto al = matjson::Value::array();
    for (int a : allowed) al.push(a);
    v["allowed"] = std::move(al);
    auto vw = matjson::Value::array();
    for (int a : viewers) vw.push(a);
    v["viewers"] = std::move(vw);
    return v;
}

void Session::saveCache(bool now) {
    if (!m_cacheDirty || m_cacheRoom.empty() || m_cacheRoom != m_roomId) return;
    (void)now;
    m_cacheDirty = false;
    auto text = cacheToJson(m_cacheRoom, m_cacheLevelName, m_cacheHeader, m_cacheMeta, m_cacheObjs,
        m_cacheAllowed, m_cacheViewers, m_cacheSavedAt).dump(matjson::NO_INDENTATION);
    auto path = cachePath();
    // se escribe aparte para no trabar el juego
    std::thread([path, text = std::move(text)] {
        auto tmp = path;
        tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return;
            f << text;
        }
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
    }).detach();
}

std::optional<matjson::Value> Session::loadCache(std::string const& roomId) {
    if (m_cacheRoom == roomId && !m_cacheObjs.empty()) {
        return cacheToJson(m_cacheRoom, m_cacheLevelName, m_cacheHeader, m_cacheMeta, m_cacheObjs,
            m_cacheAllowed, m_cacheViewers, m_cacheSavedAt);
    }
    auto text = geode::utils::file::readString(cachePath());
    if (!text) return std::nullopt;
    auto parsed = matjson::parse(text.unwrap());
    if (!parsed) return std::nullopt;
    auto v = parsed.unwrap();
    if (jstr(v, "roomId") != roomId) return std::nullopt;
    // se carga en memoria
    m_cacheRoom = roomId;
    m_cacheLevelName = jstr(v, "levelName");
    m_cacheHeader = jstr(v, "header");
    m_cacheMeta = jstr(v, "meta");
    m_cacheSavedAt = (int64_t)jnum(v, "savedAt");
    m_cacheObjs.clear();
    for (auto const& pair : jarr(v, "objects")) {
        if (!pair.isArray()) continue;
        auto const& arr = pair.asArray().unwrap();
        if (arr.size() < 2) continue;
        m_cacheObjs[arr[0].asString().unwrapOr("")] = arr[1].asString().unwrapOr("");
    }
    m_cacheAllowed.clear();
    for (auto const& a : jarr(v, "allowed")) m_cacheAllowed.push_back((int)a.asInt().unwrapOr(0));
    m_cacheViewers.clear();
    for (auto const& a : jarr(v, "viewers")) m_cacheViewers.push_back((int)a.asInt().unwrapOr(0));
    return v;
}

void Session::clearCache() {
    m_cacheRoom.clear();
    m_cacheObjs.clear();
    m_cacheHeader.clear();
    m_cacheMeta.clear();
    m_cacheAllowed.clear();
    m_cacheViewers.clear();
    m_cacheDirty = false;
    std::error_code ec;
    std::filesystem::remove(cachePath(), ec);
}

void Session::tryRevive() {
    if (m_reviving) return;
    auto roomId = m_roomId;
    auto cache = this->loadCache(roomId);
    if (!cache) {
        bool wasActive = m_active;
        this->leave(false, "la sala se perdió y no hay copia");
        if (!wasActive) saveRoomInfo("", "");
        if (wasActive) {
            FLAlertLayer::create("Collab",
                "La sala se cerró (el servidor se reinició).\nTus cambios siguen en tu copia del nivel.", "OK")->show();
        }
        return;
    }
    if (!Net::get().isReady()) {
        m_reviveRetry = 3.f;
        return;
    }
    m_reviving = true;
    auto body = std::move(*cache);
    body["state"] = this->myState();
    log::info("Collab: reviviendo la sala {}", roomId);

    Net::get().call("/room/revive", std::move(body), [this, roomId](JsonResult res) {
        m_reviving = false;
        if (m_roomId != roomId) return;
        if (!res) {
            auto err = res.unwrapErr();
            if (err.find("conectar") != std::string::npos || err.find("servidor") != std::string::npos) {
                m_reviveRetry = 5.f;
                return;
            }
            bool wasActive = m_active;
            this->leave(false, "no se pudo revivir: " + err);
            if (!wasActive) {
                saveRoomInfo("", "");
                this->clearCache();
            }
            if (wasActive) FLAlertLayer::create("Collab", "No se pudo recuperar la sala: " + err, "OK")->show();
            return;
        }
        auto snap = res.unwrap();
        this->cacheFromSnapshot(snap);
        if (!m_active) {
            this->becomeAway(roomId, jstr(snap, "levelName", m_cacheLevelName), jstr(snap, "owner"), jarr(snap, "members"));
        } else {
            m_ownerId = jstr(snap, "owner", m_ownerId);
            this->setMembers(jarr(snap, "members"));
            if (m_editor) {
                m_waitingSnapshot = true;
                m_snapshotToApply = std::move(snap);
            }
        }
        notify("La sala volvió (el servidor se había reiniciado)", NotificationIcon::Success);
    }, 240);
}

void Session::handleKicked(std::string const& by) {
    this->leave(false, "te sacaron de la sala");
    FLAlertLayer::create("Collab",
        fmt::format("<cy>{}</c> te sacó de la sala.\nTu copia del nivel sigue en tus niveles.", by.empty() ? "El anfitrión" : by),
        "OK")->show();
}

// ------------------------------------------------------------------ cargar la sala por partes

void Session::startSnapshot(matjson::Value snap, bool removeUntracked) {
    auto ed = this->editor();
    SnapJob job;
    job.removeUntracked = removeUntracked;
    if (ed) {
        auto header = jstr(snap, "header");
        if (!header.empty()) this->applyHeader(header);
        auto meta = jstr(snap, "meta");
        if (!meta.empty()) this->applyMeta(meta);
        if (ed->m_objects) {
            job.local.reserve(ed->m_objects->count());
            for (auto obj : CCArrayExt<GameObject*>(ed->m_objects)) job.local.emplace_back(obj);
        }
    }
    size_t count = snap.contains("objects") && snap["objects"].isArray()
        ? snap["objects"].asArray().unwrap().size() : 0;
    job.total = job.local.size() + count;
    job.snap = std::move(snap);
    m_job = std::move(job);
    m_waitingSnapshot = true;
    if (m_job->total > 6000) notify("Cargando la sala...");
}

int Session::loadingPercent() const {
    if (!m_job) return -1;
    return std::max(0, m_job->lastPct);
}

bool Session::stepSnapshot() {
    auto ed = this->editor();
    if (!ed) {
        m_job.reset();
        return false;
    }
    auto& job = *m_job;

    if (job.phase == 0) {
        size_t end = std::min(job.local.size(), job.index + LOAD_CHUNK);
        for (; job.index < end; job.index++) {
            auto obj = job.local[job.index].data();
            if (!obj->getParent()) continue; // lo borraste mientras cargaba
            if (!m_byPtr.contains(obj)) job.untracked.emplace(this->saveString(obj), obj);
        }
        if (job.index >= job.local.size()) {
            job.phase = 1;
            job.index = 0;
        }
    }
    else if (job.phase == 1) {
        if (!job.snap.contains("objects") || !job.snap["objects"].isArray()) {
            job.phase = 2;
        } else {
            auto const& objs = job.snap["objects"].asArray().unwrap();
            size_t end = std::min(objs.size(), job.index + LOAD_CHUNK);
            std::vector<std::pair<std::string, std::string>> toCreate;
            for (; job.index < end; job.index++) {
                auto const& pair = objs[job.index];
                if (!pair.isArray()) continue;
                auto const& arr = pair.asArray().unwrap();
                if (arr.size() < 2) continue;
                auto id = arr[0].asString().unwrapOr("");
                auto str = arr[1].asString().unwrapOr("");
                if (id.empty()) continue;
                job.keep.insert(id);

                auto it = m_byId.find(id);
                if (it != m_byId.end()) {
                    auto ptr = it->second.data();
                    auto pit = m_byPtr.find(ptr);
                    if (pit != m_byPtr.end() && pit->second.str == str) continue;
                    this->removeLocal(ptr);
                }
                auto uit = job.untracked.find(str);
                if (uit != job.untracked.end()) {
                    this->registerObject(id, uit->second, str);
                    job.untracked.erase(uit);
                    continue;
                }
                toCreate.emplace_back(std::move(id), std::move(str));
            }
            this->createBatch(toCreate);
            if (job.index >= objs.size()) job.phase = 2;
        }
    }
    else {
        // lo que tú tenías y ya no existe en la sala
        std::vector<GameObject*> remove;
        for (auto& [id, obj] : m_byId) {
            if (!job.keep.contains(id)) remove.push_back(obj.data());
        }
        if (job.removeUntracked) {
            for (auto& [str, obj] : job.untracked) {
                if (obj->getParent()) remove.push_back(obj);
            }
        }
        for (auto obj : remove) this->removeLocal(obj);
        this->purgeUndo();
        m_pending.clear();
        m_outOps.clear();
        m_held.clear();
        if (auto ui = ed->m_editorUI) {
            ui->updateButtons();
            ui->updateObjectInfoLabel();
        }
        return true;
    }

    // avance
    size_t done = job.phase == 0 ? job.index : job.local.size() + job.index;
    int pct = job.total ? (int)(done * 100 / job.total) : 100;
    if (job.total > 6000 && pct / 25 != job.lastPct / 25 && pct < 100) {
        notify(fmt::format("Cargando la sala... {}%", pct));
    }
    job.lastPct = pct;
    return false;
}

void Session::finishSnapshot() {
    auto snap = std::move(m_job->snap);
    m_job.reset();
    m_waitingSnapshot = false;
    m_ownerId = jstr(snap, "owner", m_ownerId);
    this->setMembers(jarr(snap, "members"));
    for (auto const& c : jarr(snap, "cursors")) {
        auto it = m_members.find(jstr(c, "from"));
        if (it != m_members.end()) {
            it->second.target = ccp(jnum(c, "x"), jnum(c, "y"));
            it->second.hasCursor = true;
        }
    }
    for (auto const& p : jarr(snap, "players")) this->onPlayerEvent(p);
    for (auto const& sel : jarr(snap, "selects")) {
        auto it = m_members.find(jstr(sel, "from"));
        if (it == m_members.end()) continue;
        for (auto const& id : jarr(sel, "ids")) {
            if (id.isString()) it->second.selected.push_back(id.asString().unwrap());
        }
    }
    for (auto& msg : m_chat) {
        auto it = m_members.find(msg.from);
        if (it != m_members.end()) msg.color = it->second.color;
    }
    m_sentState.clear();
    m_selSig.clear();
    notify(m_viewer ? "Estás en la sala (solo mirar)" : "Estás en la sala colaborativa", NotificationIcon::Success);
    auto early = std::move(m_earlyEvents);
    m_earlyEvents.clear();
    for (auto const& e : early) this->onEvent(e);
}

// Crea muchos objetos de una vez (mucho más rápido que de uno en uno).
void Session::createBatch(std::vector<std::pair<std::string, std::string>>& list) {
    auto ed = this->editor();
    if (!ed || list.empty()) return;
    std::string joined;
    for (auto const& [id, str] : list) {
        if (str.empty()) continue;
        joined += str;
        joined += ';';
    }
    auto arr = ed->createObjectsFromString(joined, true, true);
    size_t expected = std::count_if(list.begin(), list.end(), [](auto const& p) { return !p.second.empty(); });
    if (arr && arr->count() == expected) {
        size_t i = 0;
        for (auto const& [id, str] : list) {
            if (str.empty()) continue;
            auto obj = static_cast<GameObject*>(arr->objectAtIndex(i++));
            this->registerObject(id, obj, this->saveString(obj));
        }
    } else {
        // no coincidió: se hace de uno en uno
        if (arr) {
            for (auto obj : CCArrayExt<GameObject*>(arr)) ed->removeObject(obj, true);
        }
        for (auto const& [id, str] : list) this->createLocal(id, str);
    }
    list.clear();
}

// Ctrl+Z: se quitan del historial los objetos que cambió otra persona,
// así deshacer no revive cosas viejas ni las duplica.
void Session::purgeUndo() {
    if (m_removedForUndo.empty()) return;
    auto ed = this->editor();
    if (!ed) {
        m_removedForUndo.clear();
        return;
    }
    auto clean = [&](CCArray* list) {
        if (!list) return;
        for (int i = (int)list->count() - 1; i >= 0; i--) {
            auto undo = typeinfo_cast<UndoObject*>(list->objectAtIndex(i));
            if (!undo) continue;
            bool bad = undo->m_objectCopy && m_removedForUndo.contains(undo->m_objectCopy->m_object);
            if (!bad && undo->m_objects) {
                for (auto obj : CCArrayExt<GameObject*>(undo->m_objects)) {
                    if (m_removedForUndo.contains(obj)) {
                        bad = true;
                        break;
                    }
                }
            }
            if (bad) list->removeObjectAtIndex(i);
        }
    };
    clean(ed->m_undoObjects);
    clean(ed->m_redoObjects);
    m_removedForUndo.clear();
    if (auto ui = ed->m_editorUI) ui->updateButtons();
}

// ------------------------------------------------------------------ selección de cada uno

void Session::updateSelection(std::unordered_set<GameObject*> const& selected) {
    std::vector<std::string> ids;
    for (auto obj : selected) {
        auto it = m_byPtr.find(obj);
        if (it != m_byPtr.end()) ids.push_back(it->second.id);
        if (ids.size() >= MAX_SELECTION_SENT) break;
    }
    std::sort(ids.begin(), ids.end());
    std::string sig;
    for (auto const& id : ids) sig += id + ",";
    if (sig == m_selSig) return;
    m_selSig = sig;

    // ¿alguien más tiene seleccionado lo mismo?
    if (!ids.empty() && m_conflictCooldown <= 0.f) {
        std::unordered_set<std::string> mine(ids.begin(), ids.end());
        for (auto const& [cid, m] : m_members) {
            if (cid == Net::get().clientId() || m.state != "edit") continue;
            bool overlap = std::any_of(m.selected.begin(), m.selected.end(),
                [&](std::string const& id) { return mine.contains(id); });
            if (overlap) {
                m_conflictCooldown = 6.f;
                notify(fmt::format("{} también tiene seleccionado eso", m.username), NotificationIcon::Warning);
                break;
            }
        }
    }
    m_selPending = std::move(ids);
}

void Session::sendSelection(float dt) {
    m_selTimer += dt;
    if (!m_selPending || m_selInFlight || m_selTimer < 0.25f || !Net::get().isReady()) return;
    if (m_members.size() <= 1) {
        m_selPending.reset();
        return;
    }
    m_selTimer = 0.f;
    auto ids = matjson::Value::array();
    for (auto const& id : *m_selPending) ids.push(id);
    m_selPending.reset();
    auto body = matjson::Value::object();
    body["ids"] = std::move(ids);
    m_selInFlight = true;
    Net::get().call("/room/select", std::move(body), [this](JsonResult) {
        m_selInFlight = false;
    });
}

void Session::drawSelections(float dt) {
    m_selDrawTimer += dt;
    if (m_selDrawTimer < 0.12f) return;
    m_selDrawTimer = 0.f;
    auto ed = this->editor();
    if (!ed || !ed->m_objectLayer) return;
    float scale = ed->m_objectLayer->getScale();
    float line = scale > 0.0001f ? 1.3f / scale : 1.3f;

    for (auto& [cid, m] : m_members) {
        if (cid == Net::get().clientId()) continue;
        if (m.selected.empty() || m.state != "edit") {
            if (m.selDraw) m.selDraw->clear();
            continue;
        }
        if (!m.selDraw) {
            m.selDraw = CCDrawNode::create();
            m.selDraw->setZOrder(99998);
        }
        if (m.selDraw->getParent() != ed->m_objectLayer) {
            m.selDraw->removeFromParent();
            ed->m_objectLayer->addChild(m.selDraw, 99998);
        }
        m.selDraw->clear();
        auto c = m.color;
        ccColor4F fill = { c.r / 255.f, c.g / 255.f, c.b / 255.f, 0.12f };
        ccColor4F border = { c.r / 255.f, c.g / 255.f, c.b / 255.f, 0.95f };
        size_t drawn = 0;
        for (auto const& id : m.selected) {
            if (drawn++ >= MAX_SELECTION_DRAWN) break;
            auto it = m_byId.find(id);
            if (it == m_byId.end()) continue;
            auto rect = it->second->getObjectRect();
            CCPoint verts[] = {
                ccp(rect.getMinX(), rect.getMinY()), ccp(rect.getMaxX(), rect.getMinY()),
                ccp(rect.getMaxX(), rect.getMaxY()), ccp(rect.getMinX(), rect.getMaxY()),
            };
            m.selDraw->drawPolygon(verts, 4, fill, line, border);
        }
    }
}

// ------------------------------------------------------------------ solo mirar

void Session::viewerCheck(std::unordered_set<GameObject*> const& selected) {
    auto ed = this->editor();
    if (!ed || !ed->m_objects || m_viewerCooldown > 0.f) return;
    bool changed = ed->m_objects->count() != m_byPtr.size();
    if (!changed) {
        for (auto obj : selected) {
            auto it = m_byPtr.find(obj);
            if (it == m_byPtr.end() || this->saveString(obj) != it->second.str) {
                changed = true;
                break;
            }
        }
    }
    if (!changed && !m_lastHeader.empty() && syncSettingsEnabled() && this->settingsString(ed) != m_lastHeader) {
        changed = true;
    }
    if (!changed) return;
    m_viewerCooldown = 3.f;
    notify("Estás en modo solo mirar: tus cambios se deshacen", NotificationIcon::Warning);
    this->requestSnapshot(true);
}

// ------------------------------------------------------------------ chat, marcas e ir a

Member const* Session::member(std::string const& clientId) const {
    auto it = m_members.find(clientId);
    return it == m_members.end() ? nullptr : &it->second;
}

void Session::sendChat(std::string const& text, std::function<void(bool, std::string)> done) {
    if (!m_active) {
        done(false, "No estás en una sala.");
        return;
    }
    auto body = matjson::Value::object();
    body["text"] = text;
    Net::get().call("/room/chat", std::move(body), [done](JsonResult res) {
        done(res.isOk(), res ? "" : res.unwrapErr());
    });
}

void Session::sendPing() {
    auto ed = this->editor();
    if (!ed || !ed->m_objectLayer) {
        notify("Entra al nivel de la sala para marcar un lugar", NotificationIcon::Warning);
        return;
    }
    auto pos = ed->m_objectLayer->convertToNodeSpace(CCDirector::get()->getWinSize() / 2.f);
    auto body = matjson::Value::object();
    body["x"] = std::round(pos.x);
    body["y"] = std::round(pos.y);
    Net::get().call("/room/ping", std::move(body), nullptr);
}

void Session::goToPoint(CCPoint pos) {
    auto ed = this->editor();
    if (!ed || !ed->m_objectLayer) return;
    float scale = ed->m_objectLayer->getScale();
    auto center = CCDirector::get()->getWinSize() / 2.f;
    ed->m_objectLayer->setPosition(center - pos * scale);
}

bool Session::goToMember(std::string const& clientId) {
    auto it = m_members.find(clientId);
    if (it == m_members.end()) return false;
    auto const& m = it->second;
    if (m.ghostOn && m.state == "test" && m.ghost) {
        this->goToPoint(m.ghostTarget);
        return true;
    }
    if (m.hasCursor) {
        this->goToPoint(m.target);
        return true;
    }
    return false;
}

void Session::addMarker(CCPoint pos, std::string const& name, ccColor3B color) {
    auto ed = this->editor();
    if (!ed || !ed->m_objectLayer) return;
    auto node = CCNode::create();
    auto draw = CCDrawNode::create();
    CCPoint tri[] = { ccp(0, 0), ccp(-10, 20), ccp(10, 20) };
    ccColor4F fill = ccc4FFromccc3B(color);
    draw->drawPolygon(tri, 3, fill, 1.5f, ccc4f(0, 0, 0, 1));
    node->addChild(draw);
    auto label = CCLabelBMFont::create(fmt::format("{}: mira aquí", name).c_str(), "chatFont.fnt");
    label->setScale(0.6f);
    label->setColor(color);
    label->setPosition({0, 32});
    node->addChild(label);
    node->setPosition(pos);
    ed->m_objectLayer->addChild(node, 100001);
    m_markers.push_back(Marker{ Ref<CCNode>(node), 6.f });
}

void Session::updateMarkers(float dt) {
    auto ed = this->editor();
    float scale = ed && ed->m_objectLayer ? ed->m_objectLayer->getScale() : 1.f;
    for (auto& mk : m_markers) {
        mk.life -= dt;
        if (!mk.node) continue;
        if (scale > 0.0001f) mk.node->setScale(1.f / scale);
        // rebota un poco y se desvanece al final
        float bounce = std::abs(std::sin(mk.life * 6.f)) * 6.f;
        if (auto draw = mk.node->getChildByType<CCDrawNode>(0)) draw->setPositionY(bounce);
        if (mk.life < 1.f) mk.node->setVisible(std::fmod(mk.life, 0.2f) > 0.1f);
        if (mk.life <= 0.f) mk.node->removeFromParent();
    }
    std::erase_if(m_markers, [](Marker const& mk) { return mk.life <= 0.f; });
}

// ------------------------------------------------------------------ anfitrión

void Session::hostAction(std::string const& path, matjson::Value body, std::function<void(bool, std::string)> done) {
    if (!isOwner()) {
        done(false, "Solo el anfitrión puede hacer eso.");
        return;
    }
    Net::get().call(path, std::move(body), [done](JsonResult res) {
        done(res.isOk(), res ? "" : res.unwrapErr());
    });
}

void Session::kick(std::string const& clientId, std::function<void(bool, std::string)> done) {
    auto body = matjson::Value::object();
    body["target"] = clientId;
    this->hostAction("/room/kick", std::move(body), done);
}

void Session::makeOwner(std::string const& clientId, std::function<void(bool, std::string)> done) {
    auto body = matjson::Value::object();
    body["target"] = clientId;
    this->hostAction("/room/owner", std::move(body), done);
}

void Session::setViewer(std::string const& clientId, bool viewer, std::function<void(bool, std::string)> done) {
    auto body = matjson::Value::object();
    body["target"] = clientId;
    body["viewer"] = viewer;
    this->hostAction("/room/role", std::move(body), done);
}

} // namespace collab
