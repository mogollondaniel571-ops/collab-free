#include "InvitePopup.hpp"
#include "RoomUI.hpp"
#include "Net.hpp"
#include "Session.hpp"

#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/binding/CCMenuItemSpriteExtra.hpp>
#include <Geode/binding/GameLevelManager.hpp>
#include <Geode/binding/GJUserScore.hpp>
#include <Geode/binding/GJAccountManager.hpp>
#include <Geode/ui/Notification.hpp>

using namespace geode::prelude;

namespace collab {

static constexpr float W = 440.f;
static constexpr float H = 290.f;
static constexpr float LIST_W = 265.f;
static constexpr float LIST_H = 150.f;
static constexpr float LIST_X = 72.f;
static constexpr float MEM_W = 130.f;
static constexpr float MEM_X = -140.f;
static constexpr float PANEL_Y = 10.f;
static constexpr float ROW_H = 26.f;
static constexpr float MEM_ROW = 24.f;

InvitePopup* InvitePopup::create(LevelEditorLayer* editor, bool autoStart) {
    auto ret = new InvitePopup();
    if (ret->init(editor, autoStart)) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

InvitePopup::~InvitePopup() {
    auto glm = GameLevelManager::sharedState();
    if (glm && glm->m_userListDelegate == this) glm->m_userListDelegate = nullptr;
}

bool InvitePopup::init(LevelEditorLayer* editor, bool autoStart) {
    if (!Popup::init(W, H)) return false;
    m_editor = editor;
    this->setTitle("Collab Free");

    m_status = CCLabelBMFont::create("", "chatFont.fnt");
    m_status->setScale(0.7f);
    m_mainLayer->addChildAtPosition(m_status, Anchor::Bottom, ccp(0, 13));

    // ---------------- antes de crear la sala (o si estás en la sala de otro nivel)
    m_startNode = CCNode::create();
    m_startNode->setContentSize({W, H});
    m_startNode->setAnchorPoint({0.5f, 0.5f});
    m_mainLayer->addChildAtPosition(m_startNode, Anchor::Center);

    m_startInfo = CCLabelBMFont::create("", "chatFont.fnt");
    m_startInfo->setAlignment(kCCTextAlignmentCenter);
    m_startNode->addChildAtPosition(m_startInfo, Anchor::Center, ccp(0, 40));

    auto startMenu = CCMenu::create();
    startMenu->setContentSize({W, 60});
    startMenu->ignoreAnchorPointForPosition(false);
    startMenu->setAnchorPoint({0.5f, 0.5f});
    m_startNode->addChildAtPosition(startMenu, Anchor::Center, ccp(0, -40));
    m_startBtn = CCMenuItemSpriteExtra::create(
        ButtonSprite::create("Crear sala", "goldFont.fnt", "GJ_button_01.png", 0.9f),
        this, menu_selector(InvitePopup::onStart));
    m_startBtn->setPosition({W / 2, 30});
    startMenu->addChild(m_startBtn);
    m_leaveOtherBtn = CCMenuItemSpriteExtra::create(
        ButtonSprite::create("Salir de esa sala", "goldFont.fnt", "GJ_button_06.png", 0.7f),
        this, menu_selector(InvitePopup::onLeave));
    m_leaveOtherBtn->setPosition({W / 2, 30});
    startMenu->addChild(m_leaveOtherBtn);

    // ---------------- dentro de la sala
    m_roomNode = CCNode::create();
    m_roomNode->setContentSize({W, H});
    m_roomNode->setAnchorPoint({0.5f, 0.5f});
    m_mainLayer->addChildAtPosition(m_roomNode, Anchor::Center);

    // quién está en la sala
    m_membersTitle = CCLabelBMFont::create("En la sala", "goldFont.fnt");
    m_membersTitle->setScale(0.5f);
    m_roomNode->addChildAtPosition(m_membersTitle, Anchor::Center, ccp(MEM_X, PANEL_Y + LIST_H / 2 + 12));

    auto memBg = CCLayerColor::create({0, 0, 0, 80}, MEM_W, LIST_H);
    memBg->ignoreAnchorPointForPosition(false);
    memBg->setAnchorPoint({0.5f, 0.5f});
    m_roomNode->addChildAtPosition(memBg, Anchor::Center, ccp(MEM_X, PANEL_Y));

    m_membersList = CCNode::create();
    m_membersList->setContentSize({MEM_W, LIST_H});
    m_membersList->ignoreAnchorPointForPosition(false);
    m_membersList->setAnchorPoint({0.5f, 0.5f});
    m_roomNode->addChildAtPosition(m_membersList, Anchor::Center, ccp(MEM_X, PANEL_Y));

    // tus amigos
    auto listTitle = CCLabelBMFont::create("Tus amigos", "goldFont.fnt");
    listTitle->setScale(0.5f);
    listTitle->setAnchorPoint({0, 0.5f});
    m_roomNode->addChildAtPosition(listTitle, Anchor::Center, ccp(LIST_X - LIST_W / 2, PANEL_Y + LIST_H / 2 + 12));

    auto listBg = CCLayerColor::create({0, 0, 0, 80}, LIST_W, LIST_H);
    listBg->ignoreAnchorPointForPosition(false);
    listBg->setAnchorPoint({0.5f, 0.5f});
    m_roomNode->addChildAtPosition(listBg, Anchor::Center, ccp(LIST_X, PANEL_Y));

    m_list = ScrollLayer::create({LIST_W, LIST_H});
    m_list->ignoreAnchorPointForPosition(false);
    m_list->setAnchorPoint({0.5f, 0.5f});
    m_roomNode->addChildAtPosition(m_list, Anchor::Center, ccp(LIST_X, PANEL_Y));

    auto roomMenu = CCMenu::create();
    roomMenu->setContentSize({W, H});
    roomMenu->ignoreAnchorPointForPosition(false);
    roomMenu->setAnchorPoint({0.5f, 0.5f});
    m_roomNode->addChildAtPosition(roomMenu, Anchor::Center);

    auto refreshSpr = CCSprite::createWithSpriteFrameName("GJ_updateBtn_001.png");
    refreshSpr->setScale(0.45f);
    auto refreshBtn = CCMenuItemSpriteExtra::create(refreshSpr, this, menu_selector(InvitePopup::onRefresh));
    refreshBtn->setPosition({W / 2 + LIST_X + LIST_W / 2 - 10, H / 2 + PANEL_Y + LIST_H / 2 + 12});
    roomMenu->addChild(refreshBtn);

    m_nameInput = TextInput::create(170.f, "Nombre de usuario");
    m_nameInput->setCommonFilter(CommonFilter::Any);
    m_nameInput->setScale(0.75f);
    m_roomNode->addChildAtPosition(m_nameInput, Anchor::Center, ccp(-25, -86));

    auto inviteNameBtn = CCMenuItemSpriteExtra::create(
        ButtonSprite::create("Invitar", "goldFont.fnt", "GJ_button_01.png", 0.6f),
        this, menu_selector(InvitePopup::onInviteName));
    inviteNameBtn->setPosition({W / 2 + 100, H / 2 - 86});
    roomMenu->addChild(inviteNameBtn);

    auto leaveBtn = CCMenuItemSpriteExtra::create(
        ButtonSprite::create("Salir de la sala", "goldFont.fnt", "GJ_button_06.png", 0.5f),
        this, menu_selector(InvitePopup::onLeave));
    leaveBtn->setPosition({W / 2, H / 2 - 114});
    roomMenu->addChild(leaveBtn);

    m_wasActive = Session::get().isInEditor(m_editor);
    this->refreshState();
    this->scheduleUpdate();

    Net::get().start();
    if (autoStart && !Session::get().isActive()) {
        this->onStart(nullptr);
    }
    return true;
}

void InvitePopup::setStatus(std::string const& text, ccColor3B color) {
    m_status->setString(text.c_str());
    m_status->setColor(color);
    m_status->limitLabelWidth(W - 30.f, 0.7f, 0.3f);
}

static char const* stateText(std::string const& state) {
    if (state == "test") return "probando el nivel";
    if (state == "away") return "fuera del editor";
    return "editando";
}

void InvitePopup::buildMembers() {
    auto& s = Session::get();
    auto list = s.members();

    std::string sig = s.ownerId();
    for (auto m : list) sig += "|" + m->clientId + m->state + (m->viewer ? "v" : "");
    if (sig == m_membersSig) return;
    m_membersSig = sig;

    m_membersTitle->setString(fmt::format("En la sala ({}/6)", list.size()).c_str());
    m_membersList->removeAllChildren();
    // cada fila se puede tocar para ver las opciones (ir a, anfitrión, sacar...)
    auto rowMenu = CCMenu::create();
    rowMenu->setContentSize({MEM_W, LIST_H});
    rowMenu->setPosition({0, 0});
    m_membersList->addChild(rowMenu, 10);
    for (size_t i = 0; i < list.size(); i++) {
        auto m = list[i];
        float y = LIST_H - MEM_ROW * (i + 0.5f) - 2.f;

        if (auto dot = CCSprite::createWithSpriteFrameName("whiteCircle_001.png")) {
            dot->setScale(0.22f);
            dot->setColor(m->color);
            dot->setPosition({10, y});
            m_membersList->addChild(dot);
        }

        bool owner = m->clientId == s.ownerId();
        bool me = m->clientId == Net::get().clientId();
        auto nameText = m->username + (me ? " (tú)" : "");
        auto name = CCLabelBMFont::create(nameText.c_str(), "goldFont.fnt");
        name->setAnchorPoint({0, 0.5f});
        name->limitLabelWidth(MEM_W - 26.f, 0.42f, 0.2f);
        name->setPosition({20, y + 5});
        m_membersList->addChild(name);

        auto stateStr = std::string(stateText(m->state)) + (owner ? " · anfitrión" : (m->viewer ? " · solo mira" : ""));

        auto hit = CCLayerColor::create({255, 255, 255, 0}, MEM_W - 4.f, MEM_ROW - 2.f);
        auto hitBtn = CCMenuItemSpriteExtra::create(hit, this, menu_selector(InvitePopup::onMember));
        hitBtn->setUserObject(CCString::create(m->clientId));
        hitBtn->m_scaleMultiplier = 1.f;
        hitBtn->setPosition({MEM_W / 2, y});
        rowMenu->addChild(hitBtn);
        auto state = CCLabelBMFont::create(stateStr.c_str(), "chatFont.fnt");
        state->setAnchorPoint({0, 0.5f});
        state->limitLabelWidth(MEM_W - 26.f, 0.42f, 0.2f);
        state->setColor(m->state == "away" ? ccColor3B{170, 170, 170}
            : m->state == "test" ? ccColor3B{120, 200, 255} : ccColor3B{120, 255, 120});
        state->setPosition({20, y - 6});
        m_membersList->addChild(state);
    }
}

void InvitePopup::onMember(CCObject* sender) {
    auto str = typeinfo_cast<CCString*>(static_cast<CCNode*>(sender)->getUserObject());
    if (!str) return;
    if (auto popup = MemberPopup::create(str->getCString())) popup->show();
}

void InvitePopup::refreshState() {
    auto& s = Session::get();
    bool here = s.isInEditor(m_editor);
    bool other = s.isActive() && !here;
    m_startNode->setVisible(!here);
    m_roomNode->setVisible(here);

    if (!here) {
        if (other) {
            m_startInfo->setString(fmt::format(
                "Ya estás en la sala de \"{}\".\nSolo puedes estar en una sala a la vez:\n"
                "sal de esa para crear una aquí.", s.roomLevelName()).c_str());
        } else {
            m_startInfo->setString(
                "Crea una sala para editar este nivel\ncon hasta 5 amigos en tiempo real.\n"
                "Sirve también con niveles ya avanzados.");
        }
        m_startInfo->limitLabelWidth(W - 40.f, 0.8f, 0.3f);
        m_startBtn->setVisible(!other);
        m_leaveOtherBtn->setVisible(other);
        return;
    }
    this->buildMembers();
    if (!m_friendsLoaded) this->loadFriends();
}

void InvitePopup::update(float dt) {
    bool here = Session::get().isInEditor(m_editor);
    if (here != m_wasActive) {
        m_wasActive = here;
        if (here) this->setStatus("Sala creada. ¡Invita a tus amigos!", {120, 255, 120});
    }
    this->refreshState();

    if (!Session::get().isActive() && !Session::get().isBusy()) {
        auto& net = Net::get();
        if (m_wantStart && net.isReady()) {
            this->onStart(nullptr);
        } else if (m_wantStart && net.state() == Net::State::Connecting) {
            this->setStatus("Despertando el servidor... (puede tardar 1 o 2 minutos)", {255, 220, 120});
        } else if (m_wantStart && net.state() == Net::State::Off && !net.lastError().empty()) {
            this->setStatus(net.lastError() + " Reintentando...", {255, 120, 120});
        }
    }

    if (here && m_friendsLoaded) {
        m_onlineTimer += dt;
        if (m_onlineTimer > 8.f) {
            m_onlineTimer = 0.f;
            this->checkOnline();
        }
    }
}

void InvitePopup::onStart(CCObject*) {
    if (Session::get().isActive()) return;
    if (!m_editor || !Session::editorInScene(m_editor)) {
        this->setStatus("Abre el nivel en el editor primero.", {255, 120, 120});
        return;
    }
    if (GJAccountManager::sharedState()->m_accountID <= 0) {
        this->setStatus("Necesitas iniciar sesión en tu cuenta de Geometry Dash.", {255, 120, 120});
        return;
    }
    if (!Net::get().isReady()) {
        m_wantStart = true;
        Net::get().start();
        this->setStatus("Conectando con el servidor...", {255, 220, 120});
        return;
    }
    m_wantStart = false;
    this->setStatus("Creando sala (si el nivel es grande tarda un poco)...", {255, 220, 120});
    Ref<InvitePopup> self = this;
    Session::get().host(m_editor, [self](bool ok, std::string err) {
        if (!self->getParent()) return;
        if (ok) {
            self->setStatus("Sala creada. ¡Invita a tus amigos!", {120, 255, 120});
            self->refreshState();
        } else {
            self->setStatus(err, {255, 120, 120});
        }
    });
}

void InvitePopup::onLeave(CCObject*) {
    Ref<InvitePopup> self = this;
    createQuickPopup("Salir de la sala",
        "¿Seguro que quieres salir de la sala?\nTus amigos podrán seguir editando y tú conservas tu copia del nivel.\n"
        "Para volver te tendrán que invitar otra vez.",
        "Cancelar", "Salir",
        [self](FLAlertLayer*, bool yes) {
            if (!yes) return;
            Session::get().leave(true, "botón salir de la sala");
            Notification::create("Saliste de la sala", NotificationIcon::Info, 2.f)->show();
            self->onClose(nullptr);
        });
}

// ------------------------------------------------------------------ amigos

void InvitePopup::loadFriends() {
    m_friendsLoaded = true;
    auto glm = GameLevelManager::sharedState();
    if (auto stored = glm->getStoredUserList(UserListType::Friends)) {
        this->setFriends(stored);
        return;
    }
    this->setStatus("Cargando tu lista de amigos...", {255, 220, 120});
    glm->m_userListDelegate = this;
    glm->getUserList(UserListType::Friends);
}

void InvitePopup::getUserListFinished(CCArray* scores, UserListType type) {
    auto glm = GameLevelManager::sharedState();
    if (glm->m_userListDelegate == this) glm->m_userListDelegate = nullptr;
    if (type != UserListType::Friends) return;
    this->setStatus("");
    this->setFriends(scores);
}

void InvitePopup::getUserListFailed(UserListType type, GJErrorCode error) {
    auto glm = GameLevelManager::sharedState();
    if (glm->m_userListDelegate == this) glm->m_userListDelegate = nullptr;
    m_friends.clear();
    this->buildFriendList();
    if (error == GJErrorCode::NotFound) {
        this->setStatus("No tienes amigos agregados en GD. Invita por nombre abajo.", {255, 220, 120});
    } else {
        this->setStatus("No se pudo cargar tu lista de amigos. Invita por nombre abajo.", {255, 220, 120});
    }
}

void InvitePopup::setFriends(CCArray* scores) {
    m_friends.clear();
    if (scores) {
        for (auto score : CCArrayExt<GJUserScore*>(scores)) {
            if (score->m_accountID > 0) {
                m_friends.push_back({score->m_accountID, std::string(score->m_userName)});
            }
        }
    }
    std::sort(m_friends.begin(), m_friends.end(), [](Friend const& a, Friend const& b) {
        return a.name < b.name;
    });
    this->buildFriendList();
    this->checkOnline();
}

void InvitePopup::checkOnline() {
    if (m_checkingOnline || m_friends.empty() || !Net::get().isReady()) return;
    m_checkingOnline = true;
    auto ids = matjson::Value::array();
    for (auto const& f : m_friends) ids.push(f.accountID);
    auto body = matjson::Value::object();
    body["accountIDs"] = std::move(ids);

    Ref<InvitePopup> self = this;
    Net::get().call("/online", std::move(body), [self](JsonResult res) {
        self->m_checkingOnline = false;
        if (!res || !self->getParent()) return;
        std::unordered_set<int> online;
        for (auto const& v : jarr(res.unwrap(), "online")) {
            online.insert((int)v.asInt().unwrapOr(0));
        }
        if (online != self->m_online) {
            self->m_online = std::move(online);
            self->buildFriendList();
        }
    });
}

void InvitePopup::buildFriendList() {
    auto content = m_list->m_contentLayer;
    content->removeAllChildren();

    // primero los que tienen el mod abierto
    std::vector<Friend> sorted = m_friends;
    std::stable_sort(sorted.begin(), sorted.end(), [this](Friend const& a, Friend const& b) {
        return m_online.contains(a.accountID) > m_online.contains(b.accountID);
    });

    float totalH = std::max(LIST_H, ROW_H * sorted.size());
    content->setContentSize({LIST_W, totalH});

    if (sorted.empty()) {
        auto empty = CCLabelBMFont::create("(sin amigos para mostrar)", "chatFont.fnt");
        empty->setScale(0.7f);
        empty->setOpacity(150);
        empty->setPosition({LIST_W / 2, totalH / 2});
        content->addChild(empty);
    }

    auto menu = CCMenu::create();
    menu->setContentSize({LIST_W, totalH});
    menu->setPosition({0, 0});
    content->addChild(menu);

    for (size_t i = 0; i < sorted.size(); i++) {
        auto const& f = sorted[i];
        float y = totalH - ROW_H * (i + 0.5f);
        bool online = m_online.contains(f.accountID);

        auto name = CCLabelBMFont::create(f.name.c_str(), "goldFont.fnt");
        name->setAnchorPoint({0, 0.5f});
        name->setScale(0.5f);
        name->setPosition({10, y + 3});
        content->addChild(name);

        auto state = CCLabelBMFont::create(online ? "con el mod abierto" : "sin conexión", "chatFont.fnt");
        state->setAnchorPoint({0, 0.5f});
        state->setScale(0.5f);
        state->setColor(online ? ccColor3B{120, 255, 120} : ccColor3B{170, 170, 170});
        state->setPosition({10, y - 7});
        content->addChild(state);

        auto spr = ButtonSprite::create("Invitar", "goldFont.fnt",
            online ? "GJ_button_01.png" : "GJ_button_04.png", 0.5f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(InvitePopup::onInviteFriend));
        btn->setTag(f.accountID);
        btn->setUserObject(CCString::create(f.name));
        btn->setPosition({LIST_W - 40, y});
        menu->addChild(btn);
    }

    m_list->scrollToTop();
}

void InvitePopup::onRefresh(CCObject*) {
    m_friendsLoaded = false;
    m_online.clear();
    auto glm = GameLevelManager::sharedState();
    // forzar descarga nueva de la lista
    glm->m_userListDelegate = this;
    glm->getUserList(UserListType::Friends);
    m_friendsLoaded = true;
    this->setStatus("Actualizando...", {255, 220, 120});
}

void InvitePopup::onInviteFriend(CCObject* sender) {
    auto node = static_cast<CCNode*>(sender);
    auto nameObj = typeinfo_cast<CCString*>(node->getUserObject());
    this->doInvite(node->getTag(), nameObj ? nameObj->getCString() : "");
}

void InvitePopup::onInviteName(CCObject*) {
    auto name = m_nameInput->getString();
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    if (name.empty()) {
        this->setStatus("Escribe el nombre de usuario de tu amigo.", {255, 120, 120});
        return;
    }
    this->doInvite(0, name);
}

void InvitePopup::doInvite(int accountID, std::string const& name) {
    this->setStatus(fmt::format("Invitando a {}...", name), {255, 220, 120});
    Ref<InvitePopup> self = this;
    Session::get().invite(accountID, name, [self, name](bool ok, std::string msg) {
        if (!self->getParent()) return;
        if (ok) {
            self->setStatus(fmt::format("Invitación enviada a {}", msg.empty() ? name : msg), {120, 255, 120});
        } else {
            self->setStatus(msg, {255, 120, 120});
        }
    });
}

} // namespace collab
