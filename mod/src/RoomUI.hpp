#pragma once

// Ventanas de la sala: el chat y las acciones sobre un miembro (ir a, anfitrión, sacar...).

#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include "Session.hpp"

namespace collab {

class ChatPopup : public geode::Popup {
public:
    static ChatPopup* create();
    ~ChatPopup() override;

protected:
    bool init();
    void update(float dt) override;
    void rebuild();
    void onSend(cocos2d::CCObject*);
    void onPing(cocos2d::CCObject*);
    void onGoTo(cocos2d::CCObject* sender);

    geode::ScrollLayer* m_list = nullptr;
    geode::TextInput* m_input = nullptr;
    size_t m_shown = (size_t)-1;
    std::string m_shownLast;
    bool m_sending = false;
};

class MemberPopup : public geode::Popup {
public:
    static MemberPopup* create(std::string const& clientId);

protected:
    bool init(std::string const& clientId);
    void onGoTo(cocos2d::CCObject*);
    void onOwner(cocos2d::CCObject*);
    void onViewer(cocos2d::CCObject*);
    void onKick(cocos2d::CCObject*);
    void result(bool ok, std::string const& err, std::string const& okText);

    std::string m_clientId;
    std::string m_name;
    cocos2d::CCLabelBMFont* m_status = nullptr;
};

} // namespace collab
