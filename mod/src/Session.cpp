#include "Session.hpp"
#include "Net.hpp"

#include <Geode/binding/EditorUI.hpp>
#include <Geode/binding/GameLevelManager.hpp>
#include <Geode/binding/GJAccountManager.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/utils/cocos.hpp>
#include <Geode/utils/random.hpp>

using namespace geode::prelude;

namespace collab {

// cada cuánto se revisan los cambios (segundos)
static constexpr float SCAN_INTERVAL = 0.2f;
// cuántos objetos "no seleccionados" se revisan por vuelta en niveles grandes
static constexpr size_t SCAN_CHUNK = 1500;
// máximo de cambios por envío
static constexpr size_t MAX_OPS_PER_SEND = 3000;
static constexpr float CURSOR_INTERVAL = 0.1f;

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
    Net::get().onReconnected = [this] { this->onReconnected(); };
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

// ------------------------------------------------------------------ crear sala

void Session::host(LevelEditorLayer* editor, std::function<void(bool, std::string)> done) {
    if (m_active) {
        done(true, "");
        return;
    }
    if (m_busy) {
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
    body["header"] = m_lastHeader.empty() ? levelHeader(editor) : m_lastHeader;
    body["objects"] = std::move(objects);

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
        m_ownerId = Net::get().clientId();
        m_active = true;
        m_members.clear();
        m_colorIndex = 0;
        auto me = matjson::Value::object();
        me["client"] = Net::get().clientId();
        me["accountID"] = GJAccountManager::sharedState()->m_accountID;
        me["username"] = std::string(GJAccountManager::sharedState()->m_username);
        this->addMember(me);
        log::info("Collab: sala {} creada", m_roomId);
        done(true, "");
    });
}

void Session::invite(int accountID, std::string const& username, std::function<void(bool, std::string)> done) {
    if (!m_active) {
        done(false, "Primero crea la sala.");
        return;
    }
    auto body = matjson::Value::object();
    if (accountID > 0) body["accountID"] = accountID;
    else body["username"] = username;
    Net::get().call("/room/invite", std::move(body), [done](JsonResult res) {
        if (!res) {
            done(false, res.unwrapErr());
            return;
        }
        done(true, jstr(res.unwrap(), "username"));
    });
}

// ------------------------------------------------------------------ unirse

void Session::join(Invite const& invite) {
    if (m_busy) return;
    if (m_active) this->leave(true, "aceptaste otra invitación");
    m_busy = true;

    auto body = matjson::Value::object();
    body["roomId"] = invite.roomId;
    auto name = invite.levelName;

    Net::get().call("/room/join", std::move(body), [this, name](JsonResult res) {
        m_busy = false;
        if (!res) {
            FLAlertLayer::create("Collab", res.unwrapErr(), "OK")->show();
            return;
        }
        if (playingLevel() || currentEditor()) {
            FLAlertLayer::create("Collab", "Sal del nivel antes de unirte.", "OK")->show();
            Net::get().call("/room/leave", matjson::Value::object(), nullptr);
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
        m_joining = true;
        m_graceTimer = 0.f;
        m_earlyEvents.clear();

        auto scene = LevelEditorLayer::scene(level, false);
        CCDirector::get()->replaceScene(CCTransitionFade::create(0.5f, scene));
    });
}

bool Session::onEditorCreated(LevelEditorLayer* editor, GJGameLevel* level) {
    // abrir otro editor mientras estabas en una sala = salir de la sala
    if (m_active && editor != m_editor) {
        this->leave(true, "abriste otro editor");
    }

    if (m_joinSnapshot && m_joinLevel && m_joinLevel.data() == level) {
        auto snap = std::move(*m_joinSnapshot);
        m_joinSnapshot.reset();
        m_joinLevel = nullptr;
        m_joining = false;

        this->forgetAllObjects();
        m_editor = editor;
        m_idPrefix = randomPrefix();
        m_idCounter = 0;
        m_active = true;
        m_colorIndex = 0;
        m_graceTimer = 0.f;
        // los objetos se crean en el primer frame, con el editor ya listo
        m_snapshotToApply = std::move(snap);
        return false;
    }

    if (m_hostLevel && m_hostLevel.data() == level) {
        m_hostLevel = nullptr;
        return true; // abrir la ventana para invitar
    }
    return false;
}

// ------------------------------------------------------------------ salir

void Session::leave(bool notifyServer, std::string const& reason) {
    if (!m_active && !m_busy && !m_joining) return;
    log::info("Collab: saliendo de la sala ({})", reason.empty() ? "sin razón" : reason);
    if (notifyServer && Net::get().isReady()) {
        Net::get().call("/room/leave", matjson::Value::object(), nullptr);
    }
    for (auto& [id, m] : m_members) {
        if (m.cursor) m.cursor->removeFromParent();
    }
    m_members.clear();
    this->forgetAllObjects();
    m_active = false;
    m_busy = false;
    m_roomId.clear();
    m_ownerId.clear();
    m_editor = nullptr;
    m_sending = false;
    m_sendRetry = -1.f;
    m_sendFails = 0;
    m_needResync = false;
    m_cursorInFlight = false;
    m_snapshotToApply.reset();
    m_joining = false;
    m_joinSnapshot.reset();
    m_joinLevel = nullptr;
    m_earlyEvents.clear();
    m_lastHeader.clear();
    m_exitTimer = 0.f;
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
        for (auto const& i : m_invites) {
            if (i.roomId == inv.roomId) return;
        }
        m_invites.push_back(inv);
        return;
    }

    if (m_joining && !m_active) {
        m_earlyEvents.push_back(ev);
        return;
    }
    if (!m_active) return;
    if (m_snapshotToApply) {
        m_earlyEvents.push_back(ev);
        return;
    }

    if (type == "ops") {
        auto from = jstr(ev, "from");
        auto ops = jarr(ev, "ops");
        if (from == Net::get().clientId()) {
            // eco de lo que enviamos: ya no está pendiente
            for (auto const& op : ops) {
                auto id = jstr(op, "id");
                auto it = m_pending.find(id);
                if (it != m_pending.end() && --it->second <= 0) m_pending.erase(it);
            }
            return;
        }
        if (this->isPlaytesting() || !m_deferred.empty()) {
            m_deferred.push_back(std::move(ops));
        } else {
            this->applyRemoteOps(ops);
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
    else if (type == "join") {
        if (ev.contains("member")) {
            auto const& m = ev["member"];
            this->addMember(m);
            notify(fmt::format("{} entró a la sala", jstr(m, "username")));
        }
    }
    else if (type == "leave") {
        if (ev.contains("member")) {
            auto const& m = ev["member"];
            auto id = jstr(m, "client");
            if (id != Net::get().clientId()) {
                this->removeMember(id);
                notify(fmt::format("{} salió de la sala", jstr(m, "username")));
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
        m_needResync = true;
    }
}

void Session::onReconnected() {
    if (m_active) m_needResync = true;
}

// ------------------------------------------------------------------ sincronizar

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

    // lo seleccionado se revisa siempre (es lo que más cambia)
    std::unordered_set<GameObject*> selected;
    if (auto ui = ed->m_editorUI) {
        if (ui->m_selectedObject) selected.insert(ui->m_selectedObject);
        if (ui->m_selectedObjects) {
            for (auto obj : CCArrayExt<GameObject*>(ui->m_selectedObjects)) selected.insert(obj);
        }
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
            if (inChunk || selected.contains(obj)) {
                auto str = this->saveString(obj);
                if (str != it->second.str) {
                    it->second.str = str;
                    this->queueOp('m', it->second.id, &str);
                }
            }
        }
        index++;
    }
    if (!fullScan) m_scanCursor = chunkEnd % count;

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
        m_byId.erase(id); // suelta la referencia al final
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

    Net::get().call("/room/ops", std::move(body), [this, room, batch](JsonResult res) mutable {
        if (!m_active || room != m_roomId) return;
        m_sending = false;
        if (res) {
            m_sendFails = 0;
            this->flushOps();
            return;
        }
        // se vuelve a intentar con los mismos cambios al principio de la fila
        m_outOps.insert(m_outOps.begin(),
            std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
        m_sendFails++;
        log::warn("Collab: no se pudieron enviar cambios: {}", res.unwrapErr());
        if (m_sendFails >= 10) {
            FLAlertLayer::create("Collab",
                "Se perdió la conexión con la sala. Tus cambios siguen en tu nivel.", "OK")->show();
            this->leave(false, "no se pudieron enviar cambios");
            return;
        }
        m_sendRetry = std::min(1.f * m_sendFails, 5.f);
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

void Session::removeLocal(GameObject* obj) {
    auto ed = this->editor();
    Ref<GameObject> keep = obj;
    if (ed) {
        if (auto ui = ed->m_editorUI) {
            bool isSelected = ui->m_selectedObject == obj
                || (ui->m_selectedObjects && ui->m_selectedObjects->containsObject(obj));
            if (isSelected) ui->deselectObject(obj);
        }
        ed->removeObject(obj, true);
    }
    auto it = m_byPtr.find(obj);
    if (it != m_byPtr.end()) {
        auto id = it->second.id;
        m_byPtr.erase(it);
        m_byId.erase(id);
    }
}

void Session::applyRemoteOps(std::vector<matjson::Value> const& ops) {
    auto ed = this->editor();
    if (!ed) return;
    bool changed = false;

    for (auto const& op : ops) {
        auto id = jstr(op, "id");
        auto t = jstr(op, "t");
        if (id.empty()) continue;
        // si tú cambiaste este objeto y el servidor aún no lo recibe, gana tu cambio
        if (m_pending.contains(id)) continue;

        auto it = m_byId.find(id);
        GameObject* existing = it != m_byId.end() ? it->second.data() : nullptr;

        if (t == "h") {
            this->applyHeader(jstr(op, "s"));
            continue;
        }

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
        if (auto ui = ed->m_editorUI) {
            ui->updateButtons();
            ui->updateObjectInfoLabel();
        }
    }
}

void Session::applySnapshot(matjson::Value const& snap) {
    auto ed = this->editor();
    if (!ed) return;

    // ajustes del nivel (colores, canción, etc.)
    auto header = jstr(snap, "header");
    if (!header.empty()) this->applyHeader(header);

    std::unordered_set<std::string> keep;
    for (auto const& pair : jarr(snap, "objects")) {
        if (!pair.isArray()) continue;
        auto const& arr = pair.asArray().unwrap();
        if (arr.size() < 2) continue;
        auto id = arr[0].asString().unwrapOr("");
        auto str = arr[1].asString().unwrapOr("");
        if (id.empty()) continue;
        keep.insert(id);

        auto it = m_byId.find(id);
        if (it != m_byId.end()) {
            auto ptr = it->second.data();
            auto pit = m_byPtr.find(ptr);
            if (pit != m_byPtr.end() && pit->second.str == str) continue;
            this->removeLocal(ptr);
        }
        this->createLocal(id, str);
    }

    // lo que tú tenías y ya no existe en la sala
    std::vector<GameObject*> remove;
    for (auto& [id, obj] : m_byId) {
        if (!keep.contains(id)) remove.push_back(obj.data());
    }
    for (auto obj : remove) this->removeLocal(obj);

    m_pending.clear();
    m_outOps.clear();
    if (auto ui = ed->m_editorUI) {
        ui->updateButtons();
        ui->updateObjectInfoLabel();
    }
}

// ------------------------------------------------------------------ ajustes del nivel

std::string Session::settingsString(LevelEditorLayer* ed) {
    if (!ed || !ed->m_levelSettings) return "";
    return ed->m_levelSettings->getSaveString();
}

static bool syncSettingsEnabled() {
    return Mod::get()->getSettingValue<bool>("sync-settings");
}

// Cada segundo: si cambiaste colores, canción, velocidad, etc., se envían.
void Session::checkHeader(float dt) {
    if (!syncSettingsEnabled()) return;
    m_headerTimer += dt;
    if (m_headerTimer < 1.f) return;
    m_headerTimer = 0.f;
    auto ed = this->editor();
    if (!ed) return;
    auto now = this->settingsString(ed);
    if (now.empty() || now == m_lastHeader) return;
    m_lastHeader = now;
    this->queueOp('h', "__header__", &now);
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
    // GD lee los ajustes desde la primera parte del texto del nivel
    std::string setup = header + ";";
    ed->createObjectsFromSetup(setup);
    ed->levelSettingsUpdated();
    ed->updateLevelColors();
    m_lastHeader = this->settingsString(ed);
    log::info("Collab: ajustes del nivel actualizados");
}

// ------------------------------------------------------------------ miembros y cursores

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
    if (member.clientId.empty() || m_members.contains(member.clientId)) return;
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
    m_members.erase(it);
}

void Session::setMembers(std::vector<matjson::Value> const& list) {
    for (auto& [id, m] : m_members) {
        if (m.cursor) m.cursor->removeFromParent();
    }
    m_members.clear();
    for (auto const& m : list) this->addMember(m);
}

void Session::updateCursorNode(Member& m, float dt) {
    auto ed = this->editor();
    if (!ed || !m.cursor || !ed->m_objectLayer) return;
    if (m.cursor->getParent() != ed->m_objectLayer) {
        m.cursor->removeFromParent();
        ed->m_objectLayer->addChild(m.cursor, 100000);
    }
    m.cursor->setVisible(m.hasCursor);
    auto pos = m.cursor->getPosition();
    float k = std::min(1.f, dt * 15.f);
    m.cursor->setPosition(pos + (m.target - pos) * k);
    float scale = ed->m_objectLayer->getScale();
    if (scale > 0.0001f) m.cursor->setScale(1.f / scale);
}

void Session::sendCursor() {
    auto ed = this->editor();
    if (!ed || !ed->m_objectLayer || m_cursorInFlight || !Net::get().isReady()) return;
    if (m_members.size() <= 1) return; // nadie más lo va a ver
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

// ------------------------------------------------------------------ invitaciones

void Session::showPendingInvite() {
    if (m_invites.empty() || m_invitePopupOpen || m_busy) return;
    if (inTransition()) return;

    auto const& next = m_invites.front();
    if (playingLevel()) {
        // jugando: solo un aviso, la ventana sale cuando salgas del nivel
        if (m_notifiedInvite != next.roomId) {
            m_notifiedInvite = next.roomId;
            notify(fmt::format("{} te invitó a Collab. Sal del nivel para responder.", next.fromName));
        }
        return;
    }

    auto inv = m_invites.front();
    m_invites.pop_front();
    m_invitePopupOpen = true;
    bool inEditor = currentEditor() != nullptr;

    createQuickPopup(
        "Invitación Collab",
        fmt::format("<cy>{}</c> te invitó a editar <cg>{}</c> en tiempo real.\n{}",
            inv.fromName, inv.levelName,
            inEditor ? "Cuando guardes y salgas del editor, entrarás a la sala." : "¿Quieres unirte?"),
        "Ahora no", "Unirme",
        [this, inv](FLAlertLayer*, bool accept) {
            m_invitePopupOpen = false;
            if (!accept) return;
            if (currentEditor() || playingLevel()) {
                m_pendingJoin = inv;
                notify("Guarda y sal del editor para entrar a la sala");
            } else {
                this->join(inv);
            }
        }
    );
}

// ------------------------------------------------------------------ cada frame

void Session::tick(float dt) {
    if (!m_initialized) return;
    this->showPendingInvite();

    // invitación aceptada estando en el editor: entrar apenas salga
    if (m_pendingJoin && !m_active && !m_busy && !m_joining
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

    auto ed = this->editor();
    if (!ed) {
        // el editor puede tardar en aparecer en la escena; se espera un poco
        // antes de decidir que saliste (así no te saca por un parpadeo)
        m_exitTimer += dt;
        float limit = m_snapshotToApply ? 8.f : 1.5f;
        if (m_exitTimer > limit) {
            this->leave(true, "el editor ya no está en pantalla");
            notify("Saliste de la sala colaborativa");
        }
        return;
    }
    m_exitTimer = 0.f;

    if (m_snapshotToApply) {
        auto snap = std::move(*m_snapshotToApply);
        m_snapshotToApply.reset();
        this->applySnapshot(snap);
        this->setMembers(jarr(snap, "members"));
        for (auto const& c : jarr(snap, "cursors")) {
            auto it = m_members.find(jstr(c, "from"));
            if (it != m_members.end()) {
                it->second.target = ccp(jnum(c, "x"), jnum(c, "y"));
                it->second.hasCursor = true;
            }
        }
        notify("Entraste a la sala colaborativa", NotificationIcon::Success);
        auto early = std::move(m_earlyEvents);
        m_earlyEvents.clear();
        for (auto const& e : early) this->onEvent(e);
    }

    if (m_sendRetry >= 0.f) {
        m_sendRetry -= dt;
        if (m_sendRetry < 0.f) m_sendRetry = -1.f;
    }

    if (m_needResync && !m_busy && Net::get().isReady()) {
        m_needResync = false;
        m_busy = true;
        auto body = matjson::Value::object();
        body["roomId"] = m_roomId;
        auto room = m_roomId;
        Net::get().call("/room/join", std::move(body), [this, room](JsonResult res) {
            m_busy = false;
            if (!m_active || room != m_roomId) return;
            if (!res) {
                FLAlertLayer::create("Collab",
                    "Se perdió la sala: " + res.unwrapErr() + "\nTus cambios siguen en tu nivel.", "OK")->show();
                this->leave(false, "se perdió la sala");
                return;
            }
            auto snap = res.unwrap();
            m_deferred.clear();
            this->applySnapshot(snap);
            this->setMembers(jarr(snap, "members"));
            m_ownerId = jstr(snap, "owner");
            notify("Reconectado a la sala", NotificationIcon::Success);
        });
    }

    if (!this->isPlaytesting() && !m_busy) {
        while (!m_deferred.empty()) {
            auto ops = std::move(m_deferred.front());
            m_deferred.pop_front();
            this->applyRemoteOps(ops);
        }
        this->checkHeader(dt);
        m_scanTimer += dt;
        if (m_scanTimer >= SCAN_INTERVAL) {
            m_scanTimer = 0.f;
            this->scan();
        }
        this->flushOps();
    }

    m_cursorTimer += dt;
    if (m_cursorTimer >= CURSOR_INTERVAL) {
        m_cursorTimer = 0.f;
        this->sendCursor();
    }
    for (auto& [id, m] : m_members) this->updateCursorNode(m, dt);
}

} // namespace collab
