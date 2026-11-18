// Collab Free - edición colaborativa en tiempo real para Geometry Dash
// Ganchos del juego: botones, editor y salida del editor.

#include <Geode/Geode.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/modify/EditLevelLayer.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>
#include <Geode/modify/ProfilePage.hpp>
#include <Geode/modify/LevelSettingsLayer.hpp>
#include <Geode/binding/SongSelectNode.hpp>
#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/binding/CCMenuItemSpriteExtra.hpp>
#include <Geode/binding/GJAccountManager.hpp>

#include "Net.hpp"
#include "Session.hpp"
#include "InvitePopup.hpp"
#include "Invites.hpp"
#include "RoomUI.hpp"

using namespace geode::prelude;
using namespace collab;

// ------------------------------------------------------------------ reloj global

// Un nodo que nunca se agrega a la escena: solo recibe update() cada frame.
class CollabTicker : public CCNode {
public:
    void update(float dt) override {
        Net::get().tick(dt);
        Session::get().tick(dt);
    }
};

$on_game(Loaded) {
    Session::get().init();
    auto ticker = new CollabTicker();
    ticker->init();
    // se queda vivo para siempre (no se libera a propósito)
    CCDirector::get()->getScheduler()->scheduleUpdateForTarget(ticker, 0, false);

    if (Mod::get()->getSettingValue<bool>("receive-invites")) {
        Net::get().start();
    }
}

// ------------------------------------------------------------------ botón dentro del editor

static CCNode* makeCollabSprite(float scale) {
    auto spr = CCSprite::createWithSpriteFrameName("accountBtn_friends_001.png");
    if (!spr) spr = CCSprite::createWithSpriteFrameName("GJ_shareBtn_001.png");
    spr->setScale(scale);
    return spr;
}

class $modify(CollabEditorUI, EditorUI) {
    struct Fields {
        CCMenuItemSpriteExtra* button = nullptr;
        CCMenuItemSpriteExtra* chat = nullptr;
        CCMenuItemSpriteExtra* ping = nullptr;
        CCLabelBMFont* badge = nullptr;
        CCLabelBMFont* loading = nullptr;
    };

    bool init(LevelEditorLayer* editor) {
        if (!EditorUI::init(editor)) return false;

        auto winSize = CCDirector::get()->getWinSize();
        auto menu = CCMenu::create();
        menu->setID("collab-menu"_spr);
        menu->setPosition({0, 0});
        menu->setContentSize(winSize);

        auto btn = CCMenuItemSpriteExtra::create(
            makeCollabSprite(0.55f), this, menu_selector(CollabEditorUI::onCollab));
        btn->setID("collab-button"_spr);
        btn->setPosition({winSize.width - 105.f, winSize.height - 22.f});
        menu->addChild(btn);

        // chat de la sala (con número de mensajes sin leer)
        auto chatSpr = CCSprite::createWithSpriteFrameName("GJ_chatBtn_001.png");
        if (!chatSpr) chatSpr = CCSprite::createWithSpriteFrameName("accountBtn_messages_001.png");
        chatSpr->setScale(0.5f);
        auto badge = CCLabelBMFont::create("", "bigFont.fnt");
        badge->setScale(0.7f);
        badge->setColor({255, 90, 90});
        badge->setPosition({chatSpr->getContentSize().width - 2.f, chatSpr->getContentSize().height - 2.f});
        chatSpr->addChild(badge, 5);
        auto chatBtn = CCMenuItemSpriteExtra::create(chatSpr, this, menu_selector(CollabEditorUI::onCollabChat));
        chatBtn->setID("collab-chat"_spr);
        chatBtn->setPosition({winSize.width - 137.f, winSize.height - 22.f});
        menu->addChild(chatBtn);

        // "mira aquí": marca el centro de tu pantalla para todos
        auto pingLabel = CCLabelBMFont::create("!", "bigFont.fnt");
        auto pingSpr = CCSprite::createWithSpriteFrameName("GJ_button_02.png");
        if (pingSpr) {
            pingSpr->setScale(0.42f);
            pingLabel->setPosition(pingSpr->getContentSize() / 2);
            pingSpr->addChild(pingLabel);
        }
        CCNode* pingNode = pingSpr ? static_cast<CCNode*>(pingSpr) : static_cast<CCNode*>(pingLabel);
        auto pingBtn = CCMenuItemSpriteExtra::create(pingNode, this, menu_selector(CollabEditorUI::onCollabPing));
        pingBtn->setID("collab-ping"_spr);
        pingBtn->setPosition({winSize.width - 167.f, winSize.height - 22.f});
        menu->addChild(pingBtn);

        auto loading = CCLabelBMFont::create("", "goldFont.fnt");
        loading->setScale(0.45f);
        loading->setPosition({winSize.width / 2, winSize.height - 60.f});
        menu->addChild(loading);

        this->addChild(menu, 20);
        m_fields->button = btn;
        m_fields->chat = chatBtn;
        m_fields->ping = pingBtn;
        m_fields->badge = badge;
        m_fields->loading = loading;

        this->schedule(schedule_selector(CollabEditorUI::updateCollabButton), 0.5f);
        return true;
    }

    void updateCollabButton(float) {
        auto btn = m_fields->button;
        if (!btn) return;
        auto spr = typeinfo_cast<CCSprite*>(btn->getNormalImage());
        if (!spr) return;
        // verde cuando estás en una sala
        auto& s = Session::get();
        bool active = s.isInEditor(m_editorLayer);
        spr->setColor(active ? ccColor3B{120, 255, 140} : ccColor3B{255, 255, 255});
        if (m_fields->chat) m_fields->chat->setVisible(active);
        if (m_fields->ping) m_fields->ping->setVisible(active && !s.isViewer());
        if (m_fields->badge) {
            int unread = s.unreadChat();
            m_fields->badge->setString(unread > 0 ? fmt::format("{}", std::min(unread, 99)).c_str() : "");
        }
        if (m_fields->loading) {
            int pct = active ? s.loadingPercent() : -1;
            m_fields->loading->setString(pct >= 0 ? fmt::format("Cargando la sala... {}%", pct).c_str() : "");
        }
    }

    void onCollabChat(CCObject*) {
        if (auto popup = ChatPopup::create()) popup->show();
    }

    void onCollabPing(CCObject*) {
        Session::get().sendPing();
    }

    void onCollab(CCObject*) {
        if (auto popup = InvitePopup::create(m_editorLayer, false)) popup->show();
    }
};

// ------------------------------------------------------------------ al abrir el editor

// Espera a que termine la transición de entrada para mostrar la ventana.
static void showInviteWhenReady(Ref<LevelEditorLayer> editor, int frames) {
    geode::queueInMainThread([editor, frames] {
        if (!Session::editorInScene(editor.data()) || frames > 600) return;
        auto running = CCDirector::get()->getRunningScene();
        if (!running || typeinfo_cast<CCTransitionScene*>(running) || editor->getParent() != running) {
            showInviteWhenReady(editor, frames + 1);
            return;
        }
        if (auto popup = InvitePopup::create(editor, true)) popup->show();
    });
}

class $modify(CollabLevelEditorLayer, LevelEditorLayer) {
    bool init(GJGameLevel* level, bool noUI) {
        if (!LevelEditorLayer::init(level, noUI)) return false;
        if (noUI) return true;

        if (Session::get().onEditorCreated(this, level)) {
            // se abrió desde el botón "Collab" de la página del nivel
            showInviteWhenReady(this, 0);
        }
        return true;
    }
};

// ------------------------------------------------------------------ salir del editor

// Salir del editor NO te saca de la sala: solo se envía lo último que cambiaste.
// Para salir de la sala está el botón "Salir de la sala" en la ventana Collab.
class $modify(CollabEditorPauseLayer, EditorPauseLayer) {
    void onSaveAndExit(CCObject* sender) {
        Session::get().beforeEditorExit(m_editorLayer);
        EditorPauseLayer::onSaveAndExit(sender);
    }
    void onSaveAndPlay(CCObject* sender) {
        Session::get().beforeEditorExit(m_editorLayer);
        EditorPauseLayer::onSaveAndPlay(sender);
    }
    void onExitEditor(CCObject* sender) {
        Session::get().beforeEditorExit(m_editorLayer);
        EditorPauseLayer::onExitEditor(sender);
    }
    void onExitNoSave(CCObject* sender) {
        Session::get().beforeEditorExit(m_editorLayer);
        EditorPauseLayer::onExitNoSave(sender);
    }
};

// ------------------------------------------------------------------ ventana de ajustes del nivel

// Si tienes los ajustes abiertos mientras otro cambia la canción, al cerrarlos
// no se pisa la canción nueva con la vieja (a menos que tú la hayas cambiado).
class $modify(CollabLevelSettingsLayer, LevelSettingsLayer) {
    struct Fields {
        float openedAt = 0.f;
    };

    bool init(LevelSettingsObject* object, LevelEditorLayer* layer) {
        m_fields->openedAt = Session::get().clock();
        return LevelSettingsLayer::init(object, layer);
    }

    void onClose(CCObject* sender) {
        auto editor = m_editorLayer;
        bool touched = m_songSelectNode && m_songSelectNode->m_songChanged;
        auto& session = Session::get();
        bool watch = editor && session.isInEditor(editor);
        auto before = watch ? session.metaNow(editor) : std::string();
        auto openedAt = m_fields->openedAt;
        LevelSettingsLayer::onClose(sender);
        if (watch) session.settingsLayerClosed(editor, before, touched, openedAt);
    }
};

// ------------------------------------------------------------------ bandeja en tu perfil

class $modify(CollabProfilePage, ProfilePage) {
    struct Fields {
        CCMenuItemSpriteExtra* inbox = nullptr;
    };

    bool init(int accountID, bool ownProfile) {
        if (!ProfilePage::init(accountID, ownProfile)) return false;
        if (!ownProfile) return true;

        auto winSize = CCDirector::get()->getWinSize();
        auto menu = CCMenu::create();
        menu->setID("collab-inbox-menu"_spr);
        menu->setPosition({0, 0});
        menu->setContentSize(winSize);
        auto btn = createInboxButton(this, menu_selector(CollabProfilePage::onCollabInbox));
        btn->setPosition({winSize.width / 2 - 168.f, winSize.height / 2 + 112.f});
        menu->addChild(btn);
        m_mainLayer->addChild(menu, 20);
        m_fields->inbox = btn;
        this->schedule(schedule_selector(CollabProfilePage::updateCollabInbox), 0.5f);
        return true;
    }

    void updateCollabInbox(float) {
        updateInboxButton(m_fields->inbox);
    }

    void onCollabInbox(CCObject*) {
        if (auto popup = InboxPopup::create()) popup->show();
    }
};

// ------------------------------------------------------------------ botón en la página del nivel

class $modify(CollabEditLevelLayer, EditLevelLayer) {
    bool init(GJGameLevel* level) {
        if (!EditLevelLayer::init(level)) return false;

        auto winSize = CCDirector::get()->getWinSize();
        auto menu = CCMenu::create();
        menu->setID("collab-menu"_spr);
        menu->setPosition({0, 0});
        menu->setContentSize(winSize);

        bool inRoom = Session::get().isRoomLevel(level);
        auto spr = ButtonSprite::create(inRoom ? "Volver a la sala" : "Collab", "goldFont.fnt",
            inRoom ? "GJ_button_02.png" : "GJ_button_01.png", inRoom ? 0.55f : 0.7f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(CollabEditLevelLayer::onCollab));
        btn->setID("collab-button"_spr);
        btn->setPosition({winSize.width - (inRoom ? 75.f : 50.f), winSize.height - 25.f});
        menu->addChild(btn);
        this->addChild(menu, 10);
        return true;
    }

    void onCollab(CCObject* sender) {
        if (GJAccountManager::sharedState()->m_accountID <= 0) {
            FLAlertLayer::create("Collab",
                "Necesitas iniciar sesión en tu cuenta de Geometry Dash para usar Collab.", "OK")->show();
            return;
        }
        Net::get().start();
        auto& s = Session::get();
        if (s.isActive() && !s.isRoomLevel(m_level)) {
            FLAlertLayer::create("Collab",
                fmt::format("Ya estás en la sala de <cg>{}</c>. Solo puedes estar en una sala.\n"
                    "Para crear otra, entra a ese nivel y toca <cy>Collab</c> > <cr>Salir de la sala</c>.",
                    s.roomLevelName()),
                "OK")->show();
            return;
        }
        // si es el nivel de tu sala, solo se abre el editor y se sincroniza solo
        if (!s.isRoomLevel(m_level)) s.setHostOnNextEditor(m_level);
        // abre el editor como el botón normal de editar
        this->onEdit(sender);
    }
};
