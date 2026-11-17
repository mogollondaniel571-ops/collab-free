#pragma once

// Invitaciones: la ventanita que aparece al recibir una (con cuenta regresiva)
// y la bandeja "Invitaciones Collab" que se abre desde tu perfil.

#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include "Session.hpp"

namespace collab {

class InviteAlert : public geode::Popup {
public:
    static InviteAlert* create(Invite const& invite, bool inEditor);
    /// Cerrar sin avisar a la sesión (se acabó el tiempo o cambió de pantalla).
    void closeSilently();

protected:
    bool init(Invite const& invite, bool inEditor);
    void update(float dt) override;
    void onClose(cocos2d::CCObject*) override;
    void onLater(cocos2d::CCObject*);
    void onJoin(cocos2d::CCObject*);

    Invite m_invite;
    bool m_done = false;
    cocos2d::CCLabelBMFont* m_timer = nullptr;
};

class InboxPopup : public geode::Popup {
public:
    static InboxPopup* create();

protected:
    bool init();
    void update(float dt) override;
    void rebuild();
    void onJoin(cocos2d::CCObject* sender);
    void onDelete(cocos2d::CCObject* sender);

    geode::ScrollLayer* m_list = nullptr;
    cocos2d::CCLabelBMFont* m_info = nullptr;
    size_t m_shownCount = (size_t)-1;
    std::string m_shownRoom;
};

/// Botón con el sobre (y el número de invitaciones) para poner en menús.
CCMenuItemSpriteExtra* createInboxButton(cocos2d::CCObject* target, cocos2d::SEL_MenuHandler callback);
void updateInboxButton(CCMenuItemSpriteExtra* button);

} // namespace collab
