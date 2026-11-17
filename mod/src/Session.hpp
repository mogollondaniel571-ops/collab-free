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
    void removeInvite(std::string const& roomId);
    /// La ventanita de invitación se cerró (aceptada o "más tarde").
    void alertClosed(std::string const& roomId, bool accepted);
    float alertTimeLeft() const { return m_alertLeft; }

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

private:
    void join(Invite const& invite);
    void onEvent(matjson::Value const& ev);
    void onHello(matjson::Value const& data, bool reconnect);
    void detach(bool notifyUser);

    // sincronización de objetos
    std::string newObjectId();
    std::string saveString(GameObject* obj);
    void registerObject(std::string const& id, GameObject* obj, std::string const& str);
    void scan();
    void queueOp(char type, std::string const& id, std::string const* str);
    void flushOps();
    void applyRemoteOps(std::vector<matjson::Value> const& ops);
    void applySnapshot(matjson::Value const& snap, bool removeUntracked);
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

    std::string m_lastHeader;  // ajustes del nivel enviados/recibidos
    std::string m_lastMeta;    // canción y efectos de sonido
    int m_bgIndex = -1, m_mgIndex = -1, m_groundIndex = -1, m_lineIndex = -1;
    float m_settingsTimer = 0.f;
    float m_exitTimer = 0.f;
};

} // namespace collab
