#include "Invites.hpp"

#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/binding/CCMenuItemSpriteExtra.hpp>

using namespace geode::prelude;

namespace collab {

// ------------------------------------------------------------------ ventanita de invitación

InviteAlert* InviteAlert::create(Invite const& invite, bool inEditor) {
    auto ret = new InviteAlert();
    if (ret->init(invite, inEditor)) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

bool InviteAlert::init(Invite const& invite, bool inEditor) {
    if (!Popup::init(320.f, 170.f)) return false;
    m_invite = invite;
    this->setTitle("Invitación Collab");

    auto text = fmt::format("{} te invitó a editar\n\"{}\" en tiempo real.", invite.fromName, invite.levelName);
    auto label = CCLabelBMFont::create(text.c_str(), "chatFont.fnt");
    label->setAlignment(kCCTextAlignmentCenter);
    label->limitLabelWidth(290.f, 0.85f, 0.4f);
    m_mainLayer->addChildAtPosition(label, Anchor::Center, ccp(0, 22));

    if (inEditor) {
        auto hint = CCLabelBMFont::create("Si aceptas, entrarás cuando salgas del editor.", "chatFont.fnt");
        hint->setScale(0.55f);
        hint->setColor({255, 220, 120});
        m_mainLayer->addChildAtPosition(hint, Anchor::Center, ccp(0, -6));
    }

    m_timer = CCLabelBMFont::create("", "chatFont.fnt");
    m_timer->setScale(0.5f);
    m_timer->setOpacity(170);
    m_mainLayer->addChildAtPosition(m_timer, Anchor::Bottom, ccp(0, 12));

    auto menu = CCMenu::create();
    menu->setContentSize({320.f, 40.f});
    menu->ignoreAnchorPointForPosition(false);
    menu->setAnchorPoint({0.5f, 0.5f});
    m_mainLayer->addChildAtPosition(menu, Anchor::Bottom, ccp(0, 42));

    auto later = CCMenuItemSpriteExtra::create(
        ButtonSprite::create("Más tarde", "goldFont.fnt", "GJ_button_04.png", 0.7f),
        this, menu_selector(InviteAlert::onLater));
    later->setPosition({90.f, 20.f});
    menu->addChild(later);

    auto join = CCMenuItemSpriteExtra::create(
        ButtonSprite::create("Unirme", "goldFont.fnt", "GJ_button_01.png", 0.7f),
        this, menu_selector(InviteAlert::onJoin));
    join->setPosition({230.f, 20.f});
    menu->addChild(join);

    this->scheduleUpdate();
    this->update(0.f);
    return true;
}

void InviteAlert::update(float) {
    int left = (int)std::ceil(std::max(0.f, Session::get().alertTimeLeft()));
    m_timer->setString(fmt::format("Se guarda en tu perfil en {} s", left).c_str());
}

void InviteAlert::closeSilently() {
    m_done = true;
    Popup::onClose(nullptr);
}

void InviteAlert::onClose(CCObject* sender) {
    if (!m_done) {
        m_done = true;
        Session::get().alertClosed(m_invite.roomId, false);
    }
    Popup::onClose(sender);
}

void InviteAlert::onLater(CCObject* sender) {
    this->onClose(sender);
}

void InviteAlert::onJoin(CCObject* sender) {
    m_done = true;
    auto room = m_invite.roomId;
    Popup::onClose(sender);
    Session::get().alertClosed(room, true);
}

// ------------------------------------------------------------------ bandeja de invitaciones

static constexpr float IW = 360.f;
static constexpr float IH = 240.f;
static constexpr float LW = 320.f;
static constexpr float LH = 150.f;
static constexpr float ROW = 34.f;

InboxPopup* InboxPopup::create() {
    auto ret = new InboxPopup();
    if (ret->init()) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

bool InboxPopup::init() {
    if (!Popup::init(IW, IH)) return false;
    this->setTitle("Invitaciones Collab");

    auto bg = CCLayerColor::create({0, 0, 0, 80}, LW, LH);
    bg->ignoreAnchorPointForPosition(false);
    bg->setAnchorPoint({0.5f, 0.5f});
    m_mainLayer->addChildAtPosition(bg, Anchor::Center, ccp(0, -5));

    m_list = ScrollLayer::create({LW, LH});
    m_list->ignoreAnchorPointForPosition(false);
    m_list->setAnchorPoint({0.5f, 0.5f});
    m_mainLayer->addChildAtPosition(m_list, Anchor::Center, ccp(0, -5));

    m_info = CCLabelBMFont::create("", "chatFont.fnt");
    m_info->setScale(0.55f);
    m_info->setOpacity(190);
    m_mainLayer->addChildAtPosition(m_info, Anchor::Bottom, ccp(0, 14));

    this->rebuild();
    this->scheduleUpdate();
    return true;
}

void InboxPopup::update(float) {
    auto const& inbox = Session::get().inbox();
    std::string first = inbox.empty() ? "" : inbox.front().roomId;
    if (inbox.size() != m_shownCount || first != m_shownRoom) this->rebuild();
}

void InboxPopup::rebuild() {
    auto const& inbox = Session::get().inbox();
    m_shownCount = inbox.size();
    m_shownRoom = inbox.empty() ? "" : inbox.front().roomId;

    auto& s = Session::get();
    if (s.isActive()) {
        m_info->setString(fmt::format("Estás en la sala de \"{}\". Sal de ella para unirte a otra.",
            s.roomLevelName()).c_str());
        m_info->setColor({255, 220, 120});
    } else {
        m_info->setString("Las invitaciones se guardan mientras tengas el juego abierto.");
        m_info->setColor({255, 255, 255});
    }
    m_info->limitLabelWidth(IW - 20.f, 0.55f, 0.3f);

    auto content = m_list->m_contentLayer;
    content->removeAllChildren();
    float totalH = std::max(LH, ROW * inbox.size());
    content->setContentSize({LW, totalH});

    if (inbox.empty()) {
        auto empty = CCLabelBMFont::create("No tienes invitaciones", "chatFont.fnt");
        empty->setScale(0.7f);
        empty->setOpacity(150);
        empty->setPosition({LW / 2, totalH / 2});
        content->addChild(empty);
    }

    auto menu = CCMenu::create();
    menu->setContentSize({LW, totalH});
    menu->setPosition({0, 0});
    content->addChild(menu);

    for (size_t i = 0; i < inbox.size(); i++) {
        auto const& inv = inbox[i];
        float y = totalH - ROW * (i + 0.5f);

        auto level = CCLabelBMFont::create(inv.levelName.c_str(), "goldFont.fnt");
        level->setAnchorPoint({0, 0.5f});
        level->limitLabelWidth(170.f, 0.5f, 0.2f);
        level->setPosition({10, y + 5});
        content->addChild(level);

        auto from = CCLabelBMFont::create(fmt::format("de {}", inv.fromName).c_str(), "chatFont.fnt");
        from->setAnchorPoint({0, 0.5f});
        from->setScale(0.5f);
        from->setColor({190, 220, 255});
        from->setPosition({10, y - 8});
        content->addChild(from);

        auto joinBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Unirme", "goldFont.fnt", "GJ_button_01.png", 0.5f),
            this, menu_selector(InboxPopup::onJoin));
        joinBtn->setUserObject(CCString::create(inv.roomId));
        joinBtn->setPosition({LW - 85, y});
        menu->addChild(joinBtn);

        auto delSpr = CCSprite::createWithSpriteFrameName("GJ_deleteIcon_001.png");
        delSpr->setScale(0.6f);
        auto delBtn = CCMenuItemSpriteExtra::create(delSpr, this, menu_selector(InboxPopup::onDelete));
        delBtn->setUserObject(CCString::create(inv.roomId));
        delBtn->setPosition({LW - 22, y});
        menu->addChild(delBtn);
    }
    m_list->scrollToTop();
}

static std::string roomOf(CCObject* sender) {
    auto str = typeinfo_cast<CCString*>(static_cast<CCNode*>(sender)->getUserObject());
    return str ? str->getCString() : "";
}

void InboxPopup::onJoin(CCObject* sender) {
    auto room = roomOf(sender);
    for (auto const& inv : Session::get().inbox()) {
        if (inv.roomId == room) {
            auto copy = inv;
            bool wasActive = Session::get().isActive();
            if (!wasActive) this->onClose(nullptr);
            Session::get().accept(copy);
            return;
        }
    }
}

void InboxPopup::onDelete(CCObject* sender) {
    Session::get().removeInvite(roomOf(sender));
}

// ------------------------------------------------------------------ botón del sobre

CCMenuItemSpriteExtra* createInboxButton(CCObject* target, SEL_MenuHandler callback) {
    auto spr = CCSprite::createWithSpriteFrameName("accountBtn_messages_001.png");
    if (!spr) spr = CCSprite::createWithSpriteFrameName("GJ_chatBtn_001.png");

    auto tag = CCLabelBMFont::create("Collab", "bigFont.fnt");
    tag->setScale(0.3f);
    tag->setPosition({spr->getContentSize().width / 2, -4});
    spr->addChild(tag);

    auto badge = CCSprite::createWithSpriteFrameName("whiteCircle_001.png");
    if (badge) {
        badge->setColor({255, 60, 60});
        badge->setScale(0.45f);
        badge->setPosition({spr->getContentSize().width - 4, spr->getContentSize().height - 4});
        badge->setID("badge");
        auto count = CCLabelBMFont::create("0", "bigFont.fnt");
        count->setScale(0.9f);
        count->setPosition(badge->getContentSize() / 2);
        count->setID("count");
        badge->addChild(count);
        spr->addChild(badge, 5);
    }
    spr->setScale(0.8f);
    auto btn = CCMenuItemSpriteExtra::create(spr, target, callback);
    btn->setID("collab-inbox"_spr);
    updateInboxButton(btn);
    return btn;
}

void updateInboxButton(CCMenuItemSpriteExtra* button) {
    if (!button) return;
    auto spr = button->getNormalImage();
    if (!spr) return;
    auto badge = spr->getChildByID("badge");
    if (!badge) return;
    auto n = Session::get().inbox().size();
    badge->setVisible(n > 0);
    if (auto count = typeinfo_cast<CCLabelBMFont*>(badge->getChildByID("count"))) {
        count->setString(fmt::format("{}", n).c_str());
    }
}

} // namespace collab
