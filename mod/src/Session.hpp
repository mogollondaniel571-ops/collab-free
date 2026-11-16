#pragma once

// La sala colaborativa: detecta lo que cambias en el editor, lo envía,
// y aplica lo que cambian los demás. También maneja cursores e invitaciones.

#include <Geode/Geode.hpp>
#include <Geode/binding/LevelEditorLayer.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/GJGameLevel.hpp>
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
    cocos2d::ccColor3B color;
    geode::Ref<cocos2d::CCNode> cursor; // dibujado dentro del editor
    cocos2d::CCPoint target;            // a dónde se mueve el cursor
    bool hasCursor = false;
};

struct Invite {
    std::string roomId;
    std::string levelName;
    std::string fromName;
};

class Session {
public:
    static Session& get();

    void init();        // conecta los eventos de la red
    void tick(float dt);

    // --- estado
    bool isActive() const { return m_active; }
    bool isBusy() const { return m_busy; }
    std::string const& roomId() const { return m_roomId; }
    bool isOwner() const;
    std::vector<Member const*> members() const;
    bool isInEditor(LevelEditorLayer* editor) const;

    // --- acciones
    /// Crea una sala con el nivel que está abierto en el editor.
    void host(LevelEditorLayer* editor, std::function<void(bool, std::string)> done);
    /// Invita por número de cuenta (0 = usar el nombre).
    void invite(int accountID, std::string const& username, std::function<void(bool, std::string)> done);
    /// Acepta una invitación: descarga el nivel y abre el editor.
    void join(Invite const& invite);
    /// Sale de la sala (no borra nada del nivel).
    void leave(bool notifyServer = true);

    // --- ganchos del editor
    /// Se llama al terminar LevelEditorLayer::init.
    /// Devuelve true si hay que abrir la ventana de invitar (botón Collab del nivel).
    bool onEditorCreated(LevelEditorLayer* editor, GJGameLevel* level);
    /// Si abrir el editor desde el botón "Collab" de un nivel.
    void setHostOnNextEditor(GJGameLevel* level) { m_hostLevel = level; }

private:
    void onEvent(matjson::Value const& ev);
    void onReconnected();

    // sincronización de objetos
    std::string newObjectId();
    std::string saveString(GameObject* obj);
    void registerObject(std::string const& id, GameObject* obj, std::string const& str);
    void scan();
    void queueOp(char type, std::string const& id, std::string const* str);
    void flushOps();
    void applyRemoteOps(std::vector<matjson::Value> const& ops);
    void applySnapshot(matjson::Value const& snap);
    GameObject* createLocal(std::string const& id, std::string const& str);
    void removeLocal(GameObject* obj);
    void forgetAllObjects();
    bool isPlaytesting() const;
    LevelEditorLayer* editor() const;

    // cursores y miembros
    void setMembers(std::vector<matjson::Value> const& list);
    void addMember(matjson::Value const& m);
    void removeMember(std::string const& clientId);
    void updateCursorNode(Member& m, float dt);
    void sendCursor();
    cocos2d::ccColor3B nextColor();

    // invitaciones
    void showPendingInvite();

    bool m_initialized = false;
    bool m_active = false;
    bool m_busy = false;
    std::string m_roomId;
    std::string m_levelName;
    std::string m_ownerId;
    LevelEditorLayer* m_editor = nullptr; // se valida con LevelEditorLayer::get()

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

    // envío
    std::vector<matjson::Value> m_outOps;
    bool m_sending = false;
    float m_sendRetry = -1.f;
    int m_sendFails = 0;

    // ops recibidas mientras pruebas el nivel
    std::deque<std::vector<matjson::Value>> m_deferred;
    bool m_needResync = false;

    // miembros y cursores
    std::map<std::string, Member> m_members;
    int m_colorIndex = 0;
    float m_cursorTimer = 0.f;
    cocos2d::CCPoint m_lastCursorSent = {-99999.f, -99999.f};
    bool m_cursorInFlight = false;

    // entrar como invitado
    std::optional<matjson::Value> m_joinSnapshot;
    std::optional<matjson::Value> m_snapshotToApply; // se aplica en el primer frame del editor
    float m_graceTimer = 0.f;
    bool m_joining = false;                    // entre aceptar y abrir el editor
    std::vector<matjson::Value> m_earlyEvents; // eventos que llegan mientras tanto
    geode::Ref<GJGameLevel> m_joinLevel;
    geode::Ref<GJGameLevel> m_hostLevel;

    std::deque<Invite> m_invites;
    bool m_invitePopupOpen = false;
};

} // namespace collab
