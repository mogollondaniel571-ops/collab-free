#pragma once

// La sala colaborativa: detecta lo que cambias en el editor, lo envía,
// y aplica lo que cambian los demás. También maneja cursores, cubos e invitaciones.
//
// Estar en la sala y estar en el editor son cosas distintas: si sales del editor
// sigues en la sala ("fuera del editor") y al volver a abrir el nivel se sincroniza
// todo otra vez. Solo sales de la sala con el botón "Salir de la sala".

#include <Geode/Geode.hpp>
#include <Geode/binding/LevelEditorLayer.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/SimplePlayer.hpp>
#include <matjson.hpp>
#include <deque>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace collab {

struct Member {
    std::string clientId;
    int accountID = 0;
    std::string username;
    std::string state = "edit"; // edit, test, away
    cocos2d::ccColor3B color;
    geode::Ref<cocos2d::CCNode> cursor; // dibujado dentro del editor
    cocos2d::CCPoint target;            // a dónde se mueve el cursor
    bool hasCursor = false;

    // su cubo cuando prueba el nivel
    geode::Ref<cocos2d::CCNode> ghost;
    SimplePlayer* ghostIcon = nullptr;
    bool ghostOn = false;
    cocos2d::CCPoint ghostTarget;
    float ghostRot = 0.f;
    float ghostScale = 1.f;
    bool ghostFlip = false;
    int ghostMode = -1;
    int ghostIconId = -1;
    float ghostAge = 0.f; // segundos desde la última posición

    bool viewer = false;                        // solo puede mirar
    std::vector<std::string> selected;          // ids de lo que tiene seleccionado
    geode::Ref<cocos2d::CCDrawNode> selDraw;    // bordes de su selección
};

struct ChatMsg {
    std::string from;
    std::string username;
    std::string text;
    cocos2d::ccColor3B color = {255, 255, 255};
    bool ping = false;           // "mira aquí"
    cocos2d::CCPoint pos;        // dónde marcó
};

struct Invite {
    std::string roomId;
    std::string levelName;
    std::string fromName;
};

class Session {
public:
    static Session& get();

    /// ¿Este editor está en la escena actual (o en la transición)? No toca el puntero.
    static bool editorInScene(LevelEditorLayer* editor);
    /// El editor abierto ahora mismo (según la escena), o nullptr.
    static LevelEditorLayer* currentEditor();
    /// ¿Estás jugando un nivel?
    static bool playingLevel();
    static bool inTransition();

    void init();        // conecta los eventos de la red
    void tick(float dt);

    // --- estado
    bool isActive() const { return m_active; }
    bool isBusy() const { return m_busy; }
    std::string const& roomId() const { return m_roomId; }
    std::string const& roomLevelName() const { return m_levelName; }
    bool isOwner() const;
    std::string const& ownerId() const { return m_ownerId; }
    std::vector<Member const*> members() const;
    /// ¿Estás en la sala Y este es el editor que se está sincronizando?
    bool isInEditor(LevelEditorLayer* editor) const;

    // --- acciones
    /// Crea una sala con el nivel que está abierto en el editor.
    void host(LevelEditorLayer* editor, std::function<void(bool, std::string)> done);
    /// Invita por número de cuenta (0 = usar el nombre).
    void invite(int accountID, std::string const& username, std::function<void(bool, std::string)> done);
    /// Acepta una invitación (si estás en el editor, entra al salir).
    void accept(Invite const& invite);
    /// Sale de la sala (no borra nada del nivel).
    void leave(bool notifyServer = true, std::string const& reason = "");

    // --- invitaciones guardadas
    std::vector<Invite> const& inbox() const { return m_inbox; }
    void removeInvite(std::string const& roomId, bool tellServer = false);
    /// La ventanita de invitación se cerró (aceptada o "más tarde").
    void alertClosed(std::string const& roomId, bool accepted);
    float alertTimeLeft() const { return m_alertLeft; }

    /// Canción del nivel tal como está ahora (para la ventana de ajustes).
    std::string metaNow(LevelEditorLayer* editor);
    /// Se cerró la ventana de ajustes del nivel.
    void settingsLayerClosed(LevelEditorLayer* editor, std::string const& metaBefore, bool songTouched, float openedAt);
    float clock() const { return m_clock; }

    /// Antes de salir del editor: envía lo último que cambiaste.
    void beforeEditorExit(LevelEditorLayer* editor);

    // --- ganchos del editor
    /// Se llama al terminar LevelEditorLayer::init.
    /// Devuelve true si hay que abrir la ventana de invitar (botón Collab del nivel).
    bool onEditorCreated(LevelEditorLayer* editor, GJGameLevel* level);
    /// Si abrir el editor desde el botón "Collab" de un nivel.
    void setHostOnNextEditor(GJGameLevel* level) { m_hostLevel = level; }
    /// ¿Este nivel es el de tu sala?
    bool isRoomLevel(GJGameLevel* level) const;

    // --- chat y marcas
    std::deque<ChatMsg> const& chat() const { return m_chat; }
    int unreadChat() const { return m_unread; }
    void setChatOpen(bool open) { m_chatOpen = open; if (open) m_unread = 0; }
    void sendChat(std::string const& text, std::function<void(bool, std::string)> done);
    /// "Mira aquí": marca el centro de tu pantalla.
    void sendPing();
    /// Mueve tu cámara a donde está esa persona. false si no se sabe dónde está.
    bool goToMember(std::string const& clientId);
    void goToPoint(cocos2d::CCPoint pos);
    Member const* member(std::string const& clientId) const;

    // --- anfitrión
    bool isViewer() const { return m_viewer; }
    void kick(std::string const& clientId, std::function<void(bool, std::string)> done);
    void makeOwner(std::string const& clientId, std::function<void(bool, std::string)> done);
    void setViewer(std::string const& clientId, bool viewer, std::function<void(bool, std::string)> done);

    /// 0..100 mientras se carga una sala grande, -1 si no.
    int loadingPercent() const;

private:
    void join(Invite const& invite);
    void onEvent(matjson::Value const& ev);
    void onHello(matjson::Value const& data, bool reconnect);
    void detach(bool notifyUser);
    void becomeAway(std::string const& roomId, std::string const& levelName,
        std::string const& owner, std::vector<matjson::Value> const& members);
    void handleKicked(std::string const& by);

    // copia local de la sala (para revivirla si el servidor se reinicia)
    void cacheFromSnapshot(matjson::Value const& snap);
    void cacheApplyOps(std::vector<matjson::Value> const& ops);
    void cacheTouch();
    void saveCache(bool now);
    std::optional<matjson::Value> loadCache(std::string const& roomId);
    void clearCache();
    void tryRevive();

    // cargar la sala por partes (niveles grandes)
    void startSnapshot(matjson::Value snap, bool removeUntracked);
    bool stepSnapshot();
    void finishSnapshot();
    void createBatch(std::vector<std::pair<std::string, std::string>>& list);
    void purgeUndo();

    // selección, marcas, solo mirar
    void updateSelection(std::unordered_set<GameObject*> const& selected);
    void sendSelection(float dt);
    void drawSelections(float dt);
    void addMarker(cocos2d::CCPoint pos, std::string const& name, cocos2d::ccColor3B color);
    void updateMarkers(float dt);
    void viewerCheck(std::unordered_set<GameObject*> const& selected);
    void hostAction(std::string const& path, matjson::Value body, std::function<void(bool, std::string)> done);

    // sincronización de objetos
    std::string newObjectId();
    std::string saveString(GameObject* obj);
    void registerObject(std::string const& id, GameObject* obj, std::string const& str);
    void scan();
    void queueOp(char type, std::string const& id, std::string const* str);
    void flushOps();
    void applyRemoteOps(std::vector<matjson::Value> const& ops, std::string const& fromName = "");
    void requestSnapshot(bool removeUntracked);
    GameObject* createLocal(std::string const& id, std::string const& str);
    void removeLocal(GameObject* obj);
    void forgetAllObjects();
    bool isPlaytesting() const;
    LevelEditorLayer* editor() const;

    // ajustes del nivel, fondo y canción
    std::string settingsString(LevelEditorLayer* ed);
    std::string metaString(LevelEditorLayer* ed);
    void applyHeader(std::string const& header);
    void applyMeta(std::string const& meta);
    void restoreMeta(LevelEditorLayer* ed, std::string const& meta);
    void checkSettings(float dt);

    // miembros, cursores y cubos
    void setMembers(std::vector<matjson::Value> const& list);
    void addMember(matjson::Value const& m);
    void removeMember(std::string const& clientId);
    void clearMemberNodes();
    void updateCursorNode(Member& m, float dt);
    void updateGhostNode(Member& m, float dt);
    void onPlayerEvent(matjson::Value const& ev);
    void sendCursor();
    void sendPlayer(float dt);
    void sendStatus();
    std::string myState() const;
    cocos2d::ccColor3B nextColor();

    // invitaciones
    void updateInviteAlert(float dt);

    bool m_initialized = false;
    bool m_active = false;
    bool m_busy = false;
    std::string m_roomId;
    std::string m_levelName;
    std::string m_ownerId;
    LevelEditorLayer* m_editor = nullptr;  // se valida con editorInScene()
    geode::Ref<GJGameLevel> m_roomLevel;   // tu copia del nivel de la sala

    // objetos: id <-> objeto
    struct Tracked {
        std::string id;
        std::string str;
        uint32_t seen = 0;
    };
    std::unordered_map<std::string, geode::Ref<GameObject>> m_byId;
    std::unordered_map<GameObject*, Tracked> m_byPtr;
    std::unordered_map<std::string, int> m_pending; // ops propias sin eco
    std::string m_idPrefix;
    uint64_t m_idCounter = 0;
    uint32_t m_scanTick = 0;
    size_t m_scanCursor = 0;
    float m_scanTimer = 0.f;

    // cambios de otros a objetos que tienes seleccionados (se aplican al soltarlos)
    std::map<std::string, matjson::Value> m_held;
    // objetos borrados que se guardan un rato por si alguna ventana los usa
    std::vector<geode::Ref<GameObject>> m_graveyard;
    float m_graveyardTimer = 0.f;

    // envío
    std::vector<matjson::Value> m_outOps;
    bool m_sending = false;
    float m_sendRetry = -1.f;
    int m_sendFails = 0;

    // ops recibidas mientras pruebas el nivel o esperas la copia de la sala
    std::deque<std::vector<matjson::Value>> m_deferred;
    bool m_needResync = false;
    bool m_resyncRemoveUntracked = false;
    int m_resyncFails = 0;
    float m_resyncDelay = 0.f;
    bool m_waitingSnapshot = false; // no se revisan cambios hasta tener la copia

    // miembros, cursores y cubos
    std::map<std::string, Member> m_members;
    int m_colorIndex = 0;
    float m_cursorTimer = 0.f;
    cocos2d::CCPoint m_lastCursorSent = {-99999.f, -99999.f};
    bool m_cursorInFlight = false;
    float m_playerTimer = 0.f;
    bool m_playerInFlight = false;
    bool m_playerSentOn = false;
    std::string m_sentState;
    bool m_statusInFlight = false;

    // entrar como invitado
    std::optional<matjson::Value> m_joinSnapshot;
    std::optional<matjson::Value> m_snapshotToApply; // se aplica en el primer frame del editor
    float m_graceTimer = 0.f;
    bool m_joining = false;                    // entre aceptar y abrir el editor
    std::vector<matjson::Value> m_earlyEvents; // eventos que llegan mientras tanto
    geode::Ref<GJGameLevel> m_joinLevel;
    geode::Ref<GJGameLevel> m_hostLevel;
    std::optional<Invite> m_pendingJoin; // aceptada estando en el editor

    // invitaciones
    std::vector<Invite> m_inbox;              // guardadas (se ven en tu perfil)
    std::deque<Invite> m_alertQueue;          // por mostrar en ventanita
    std::optional<Invite> m_alert;            // la que se está mostrando
    float m_alertLeft = 0.f;                  // segundos que le quedan en pantalla
    geode::Ref<cocos2d::CCNode> m_alertNode;  // la ventanita (se vuelve a abrir si cambias de pantalla)
    std::string m_notifiedInvite;             // para no repetir el aviso

    // chat y marcas
    std::deque<ChatMsg> m_chat;
    int m_unread = 0;
    bool m_chatOpen = false;
    struct Marker { geode::Ref<cocos2d::CCNode> node; float life = 0.f; };
    std::vector<Marker> m_markers;

    // selección
    std::string m_selSig;
    std::optional<std::vector<std::string>> m_selPending;
    bool m_selInFlight = false;
    float m_selTimer = 0.f;
    float m_selDrawTimer = 0.f;
    float m_conflictCooldown = 0.f;

    // solo mirar
    bool m_viewer = false;
    float m_viewerCooldown = 0.f;

    // objetos quitados por cambios de otros (se limpian del Ctrl+Z)
    std::unordered_set<GameObject*> m_removedForUndo;

    // carga por partes
    struct SnapJob {
        matjson::Value snap;
        bool removeUntracked = false;
        int phase = 0;
        size_t index = 0;
        std::vector<geode::Ref<GameObject>> local;
        std::unordered_multimap<std::string, GameObject*> untracked;
        std::unordered_set<std::string> keep;
        size_t total = 0;
        int lastPct = -1;
    };
    std::optional<SnapJob> m_job;

    // copia local de la sala
    std::string m_cacheRoom;
    std::string m_cacheLevelName;
    std::unordered_map<std::string, std::string> m_cacheObjs;
    std::string m_cacheHeader;
    std::string m_cacheMeta;
    std::vector<int> m_cacheAllowed;
    std::vector<int> m_cacheViewers;
    int64_t m_cacheSavedAt = 0;
    bool m_cacheDirty = false;
    float m_cacheTimer = 0.f;
    bool m_reviving = false;
    float m_reviveRetry = -1.f;

    std::string m_lastHeader;  // ajustes del nivel enviados/recibidos
    std::string m_lastMeta;    // canción y efectos de sonido
    int m_bgIndex = -1, m_mgIndex = -1, m_groundIndex = -1, m_lineIndex = -1;
    float m_settingsTimer = 0.f;
    float m_clock = 0.f;        // segundos desde que abriste el juego
    float m_metaRemoteAt = -1.f;
    float m_exitTimer = 0.f;
};

} // namespace collab
