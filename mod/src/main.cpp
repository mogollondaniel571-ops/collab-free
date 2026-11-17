// Collab Free - edición colaborativa en tiempo real para Geometry Dash
// Ganchos del juego: botones, editor y salida del editor.

#include <Geode/Geode.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/modify/EditLevelLayer.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>
#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/binding/CCMenuItemSpriteExtra.hpp>
#include <Geode/binding/GJAccountManager.hpp>

#include "Net.hpp"
#include "Session.hpp"
#include "InvitePopup.hpp"

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
        this->addChild(menu, 20);
        m_fields->button = btn;

        this->schedule(schedule_selector(CollabEditorUI::updateCollabButton), 0.5f);
        return true;
    }

    void updateCollabButton(float) {
        auto btn = m_fields->button;
        if (!btn) return;
        auto spr = typeinfo_cast<CCSprite*>(btn->getNormalImage());
        if (!spr) return;
        // verde cuando estás en una sala
        bool active = Session::get().isInEditor(m_editorLayer);
        spr->setColor(active ? ccColor3B{120, 255, 140} : ccColor3B{255, 255, 255});
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

class $modify(CollabEditorPauseLayer, EditorPauseLayer) {
    void onSaveAndExit(CCObject* sender) {
        Session::get().leave(true, "guardar y salir");
        EditorPauseLayer::onSaveAndExit(sender);
    }
    void onSaveAndPlay(CCObject* sender) {
        Session::get().leave(true, "guardar y jugar");
        EditorPauseLayer::onSaveAndPlay(sender);
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

        auto spr = ButtonSprite::create("Collab", "goldFont.fnt", "GJ_button_01.png", 0.7f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(CollabEditLevelLayer::onCollab));
        btn->setID("collab-button"_spr);
        btn->setPosition({winSize.width - 50.f, winSize.height - 25.f});
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
        Session::get().setHostOnNextEditor(m_level);
        // abre el editor como el botón normal de editar
        this->onEdit(sender);
    }
};
