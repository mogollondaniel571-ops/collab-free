#pragma once

// Ventana "Collab": crear la sala, ver quién está, e invitar amigos.

#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/binding/UserListDelegate.hpp>
#include <Geode/binding/LevelEditorLayer.hpp>
#include <unordered_set>

namespace collab {

class InvitePopup : public geode::Popup, public UserListDelegate {
public:
    /// autoStart = crear la sala apenas se abre
    static InvitePopup* create(LevelEditorLayer* editor, bool autoStart);

protected:
    bool init(LevelEditorLayer* editor, bool autoStart);
    ~InvitePopup() override;

    void update(float dt) override;
    void refreshState();

    // sala
    void onStart(cocos2d::CCObject*);
    void onLeave(cocos2d::CCObject*);

    // amigos
    void loadFriends();
    void getUserListFinished(cocos2d::CCArray* scores, UserListType type) override;
    void getUserListFailed(UserListType type, GJErrorCode error) override;
    void setFriends(cocos2d::CCArray* scores);
    void checkOnline();
    void buildFriendList();
    void onInviteFriend(cocos2d::CCObject* sender);
    void onInviteName(cocos2d::CCObject*);
    void onRefresh(cocos2d::CCObject*);
    void doInvite(int accountID, std::string const& name);

    void setStatus(std::string const& text, cocos2d::ccColor3B color = {255, 255, 255});

    LevelEditorLayer* m_editor = nullptr;
    bool m_wasActive = false;
    bool m_wantStart = false; // crear la sala apenas haya conexión
    float m_onlineTimer = 0.f;

    cocos2d::CCNode* m_startNode = nullptr;
    cocos2d::CCNode* m_roomNode = nullptr;
    cocos2d::CCLabelBMFont* m_status = nullptr;
    cocos2d::CCLabelBMFont* m_membersLabel = nullptr;
    geode::ScrollLayer* m_list = nullptr;
    geode::TextInput* m_nameInput = nullptr;
    CCMenuItemSpriteExtra* m_startBtn = nullptr;

    struct Friend {
        int accountID;
        std::string name;
    };
    std::vector<Friend> m_friends;
    std::unordered_set<int> m_online;
    bool m_friendsLoaded = false;
    bool m_checkingOnline = false;
};

} // namespace collab
