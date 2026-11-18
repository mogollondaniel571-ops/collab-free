#include "RoomUI.hpp"
#include "Net.hpp"

#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/binding/CCMenuItemSpriteExtra.hpp>
#include <Geode/ui/Notification.hpp>

using namespace geode::prelude;

namespace collab {

// ------------------------------------------------------------------ chat

static constexpr float CW = 390.f;
static constexpr float CH = 260.f;
static constexpr float CLW = 350.f;
static constexpr float CLH = 150.f;
static constexpr float LINE = 17.f;
static constexpr size_t WRAP = 52;

ChatPopup* ChatPopup::create() {
    auto ret = new ChatPopup();
    if (ret->init()) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

ChatPopup::~ChatPopup() {
    Session::get().setChatOpen(false);
}

bool ChatPopup::init() {
    if (!Popup::init(CW, CH)) return false;
    this->setTitle("Chat de la sala");
    Session::get().setChatOpen(true);

    auto bg = CCLayerColor::create({0, 0, 0, 90}, CLW, CLH);
    bg->ignoreAnchorPointForPosition(false);
    bg->setAnchorPoint({0.5f, 0.5f});
    m_mainLayer->addChildAtPosition(bg, Anchor::Center, ccp(0, 8));

    m_list = ScrollLayer::create({CLW, CLH});
    m_list->ignoreAnchorPointForPosition(false);
    m_list->setAnchorPoint({0.5f, 0.5f});
    m_mainLayer->addChildAtPosition(m_list, Anchor::Center, ccp(0, 8));

    m_input = TextInput::create(250.f, "Escribe un mensaje...");
    m_input->setCommonFilter(CommonFilter::Any);
    m_input->setMaxCharCount(200);
    m_input->setScale(0.8f);
    m_mainLayer->addChildAtPosition(m_input, Anchor::Bottom, ccp(-45, 28));

    auto menu = CCMenu::create();
    menu->setContentSize({CW, CH});
    menu->ignoreAnchorPointForPosition(false);
    menu->setAnchorPoint({0.5f, 0.5f});
    m_mainLayer->addChildAtPosition(menu, Anchor::Center);

    auto send = CCMenuItemSpriteExtra::create(
        ButtonSprite::create("Enviar", "goldFont.fnt", "GJ_button_01.png", 0.6f),
        this, menu_selector(ChatPopup::onSend));
    send->setPosition({CW / 2 + 130, 28});
    menu->addChild(send);

    auto ping = CCMenuItemSpriteExtra::create(
        ButtonSprite::create("Marcar aquí", "goldFont.fnt", "GJ_button_02.png", 0.45f),
        this, menu_selector(ChatPopup::onPing));
    ping->setPosition({CW - 55, CH - 22});
    menu->addChild(ping);

    this->rebuild();
    this->scheduleUpdate();
    return true;
}

void ChatPopup::update(float) {
    auto const& chat = Session::get().chat();
    std::string last = chat.empty() ? "" : chat.back().username + chat.back().text;
    if (chat.size() != m_shown || last != m_shownLast) this->rebuild();
}

// corta un mensaje largo en varias líneas
static std::vector<std::string> wrapText(std::string const& text) {
    std::vector<std::string> lines;
    std::string line;
    size_t start = 0;
    while (start < text.size()) {
        auto space = text.find(' ', start);
        auto word = text.substr(start, space == std::string::npos ? std::string::npos : space - start);
        start = space == std::string::npos ? text.size() : space + 1;
        while (word.size() > WRAP) {
            if (!line.empty()) {
                lines.push_back(line);
                line.clear();
            }
            lines.push_back(word.substr(0, WRAP));
            word = word.substr(WRAP);
        }
        if (line.size() + word.size() + 1 > WRAP && !line.empty()) {
            lines.push_back(line);
            line.clear();
        }
        if (!line.empty()) line += ' ';
        line += word;
    }
    if (!line.empty()) lines.push_back(line);
    if (lines.empty()) lines.push_back("");
    return lines;
}

void ChatPopup::rebuild() {
    auto const& chat = Session::get().chat();
    m_shown = chat.size();
    m_shownLast = chat.empty() ? "" : chat.back().username + chat.back().text;

    struct Row {
        std::string name;
        std::string text;
        ccColor3B color;
        bool ping = false;
        size_t index = 0;
    };
    std::vector<Row> rows;
    for (size_t i = 0; i < chat.size(); i++) {
        auto const& msg = chat[i];
        if (msg.ping) {
            rows.push_back({ msg.username, "marcó un lugar", msg.color, true, i });
            continue;
        }
        auto lines = wrapText(msg.text);
        for (size_t l = 0; l < lines.size(); l++) {
            rows.push_back({ l == 0 ? msg.username : "", lines[l], msg.color, false, i });
        }
    }

    auto content = m_list->m_contentLayer;
    content->removeAllChildren();
    float totalH = std::max(CLH, LINE * rows.size() + 6.f);
    content->setContentSize({CLW, totalH});

    if (rows.empty()) {
        auto empty = CCLabelBMFont::create("Todavía no hay mensajes", "chatFont.fnt");
        empty->setScale(0.65f);
        empty->setOpacity(140);
        empty->setPosition({CLW / 2, totalH / 2});
        content->addChild(empty);
    }

    auto menu = CCMenu::create();
    menu->setContentSize({CLW, totalH});
    menu->setPosition({0, 0});
    content->addChild(menu);

    for (size_t i = 0; i < rows.size(); i++) {
        auto const& row = rows[i];
        float y = totalH - 3.f - LINE * (i + 0.5f);
        float x = 8.f;
        if (!row.name.empty()) {
            auto name = CCLabelBMFont::create((row.name + ":").c_str(), "chatFont.fnt");
            name->setAnchorPoint({0, 0.5f});
            name->setScale(0.6f);
            name->setColor(row.color);
            name->setPosition({x, y});
            content->addChild(name);
            x += name->getScaledContentSize().width + 5.f;
        } else {
            x += 14.f;
        }
        auto text = CCLabelBMFont::create(row.text.c_str(), "chatFont.fnt");
        text->setAnchorPoint({0, 0.5f});
        text->setScale(0.6f);
        if (row.ping) text->setColor({255, 220, 120});
        text->limitLabelWidth(CLW - x - (row.ping ? 50.f : 8.f), 0.6f, 0.3f);
        text->setPosition({x, y});
        content->addChild(text);

        if (row.ping) {
            auto go = CCMenuItemSpriteExtra::create(
                ButtonSprite::create("Ir", "goldFont.fnt", "GJ_button_01.png", 0.4f),
                this, menu_selector(ChatPopup::onGoTo));
            go->setTag((int)row.index);
            go->setPosition({CLW - 22.f, y});
            menu->addChild(go);
        }
    }
    // siempre se ve lo último
    m_list->scrollToTop();
    m_list->m_contentLayer->setPositionY(0);
}

void ChatPopup::onSend(CCObject*) {
    auto text = m_input->getString();
    while (!text.empty() && text.back() == ' ') text.pop_back();
    if (text.empty() || m_sending) return;
    m_sending = true;
    Ref<ChatPopup> self = this;
    Session::get().sendChat(text, [self](bool ok, std::string err) {
        self->m_sending = false;
        if (ok) {
            self->m_input->setString("");
        } else {
            Notification::create(err, NotificationIcon::Error, 2.f)->show();
        }
    });
}

void ChatPopup::onPing(CCObject*) {
    Session::get().sendPing();
    this->onClose(nullptr);
}

void ChatPopup::onGoTo(CCObject* sender) {
    auto const& chat = Session::get().chat();
    auto index = (size_t)static_cast<CCNode*>(sender)->getTag();
    if (index >= chat.size()) return;
    auto pos = chat[index].pos;
    this->onClose(nullptr);
    Session::get().goToPoint(pos);
}

// ------------------------------------------------------------------ acciones de un miembro

static constexpr float MW = 280.f;
static constexpr float MH = 230.f;

MemberPopup* MemberPopup::create(std::string const& clientId) {
    auto ret = new MemberPopup();
    if (ret->init(clientId)) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

bool MemberPopup::init(std::string const& clientId) {
    if (!Popup::init(MW, MH)) return false;
    auto& s = Session::get();
    auto m = s.member(clientId);
    if (!m) return false;
    m_clientId = clientId;
    m_name = m->username;
    this->setTitle(m_name.c_str());

    bool me = clientId == Net::get().clientId();
    bool owner = s.ownerId() == clientId;
    std::string info = owner ? "Anfitrión de la sala" : (m->viewer ? "Solo puede mirar" : "Puede editar");
    auto infoLabel = CCLabelBMFont::create(info.c_str(), "chatFont.fnt");
    infoLabel->setScale(0.65f);
    infoLabel->setColor(m->color);
    m_mainLayer->addChildAtPosition(infoLabel, Anchor::Top, ccp(0, -45));

    m_status = CCLabelBMFont::create("", "chatFont.fnt");
    m_status->setScale(0.55f);
    m_mainLayer->addChildAtPosition(m_status, Anchor::Bottom, ccp(0, 12));

    auto menu = CCMenu::create();
    menu->setContentSize({MW, MH - 80.f});
    menu->ignoreAnchorPointForPosition(false);
    menu->setAnchorPoint({0.5f, 0.5f});
    m_mainLayer->addChildAtPosition(menu, Anchor::Center, ccp(0, -14));

    int count = 0;
    auto add = [&](char const* text, char const* bg, SEL_MenuHandler sel) {
        auto btn = CCMenuItemSpriteExtra::create(ButtonSprite::create(text, "goldFont.fnt", bg, 0.6f), this, sel);
        btn->setPosition({MW / 2, (MH - 80.f) - 18.f - 34.f * count});
        menu->addChild(btn);
        count++;
    };
    if (!me) add("Ir a donde está", "GJ_button_01.png", menu_selector(MemberPopup::onGoTo));
    if (s.isOwner() && !me) {
        add("Pasarle el mando", "GJ_button_02.png", menu_selector(MemberPopup::onOwner));
        add(m->viewer ? "Dejarlo editar" : "Solo mirar", "GJ_button_04.png", menu_selector(MemberPopup::onViewer));
        add("Sacar de la sala", "GJ_button_06.png", menu_selector(MemberPopup::onKick));
    }
    if (menu->getChildrenCount() == 0) {
        auto none = CCLabelBMFont::create(me ? "Eres tú" : "Solo el anfitrión puede\ncambiar los permisos.", "chatFont.fnt");
        none->setScale(0.6f);
        none->setAlignment(kCCTextAlignmentCenter);
        m_mainLayer->addChildAtPosition(none, Anchor::Center, ccp(0, -10));
    }
    return true;
}

void MemberPopup::result(bool ok, std::string const& err, std::string const& okText) {
    if (ok) {
        Notification::create(okText, NotificationIcon::Success, 2.f)->show();
        this->onClose(nullptr);
    } else {
        m_status->setString(err.c_str());
        m_status->setColor({255, 120, 120});
    }
}

void MemberPopup::onGoTo(CCObject*) {
    if (Session::get().goToMember(m_clientId)) {
        this->onClose(nullptr);
        // también se cierra la ventana Collab de atrás
        if (auto scene = CCDirector::get()->getRunningScene()) {
            std::vector<Ref<geode::Popup>> popups;
            for (auto child : CCArrayExt<CCNode*>(scene->getChildren())) {
                if (auto popup = typeinfo_cast<geode::Popup*>(child)) popups.emplace_back(popup);
            }
            for (auto& popup : popups) popup->onClose(nullptr);
        }
    } else {
        m_status->setString("Todavía no se sabe dónde está (¿está fuera del editor?)");
        m_status->setColor({255, 220, 120});
    }
}

void MemberPopup::onOwner(CCObject*) {
    Ref<MemberPopup> self = this;
    auto name = m_name;
    createQuickPopup("Pasar el mando",
        fmt::format("¿Hacer a <cy>{}</c> el anfitrión? Tú ya no podrás sacar gente ni cambiar permisos.", name),
        "Cancelar", "Sí",
        [self, name](FLAlertLayer*, bool yes) {
            if (!yes) return;
            Session::get().makeOwner(self->m_clientId, [self, name](bool ok, std::string err) {
                self->result(ok, err, name + " ahora es el anfitrión");
            });
        });
}

void MemberPopup::onViewer(CCObject*) {
    auto m = Session::get().member(m_clientId);
    if (!m) return;
    bool viewer = !m->viewer;
    Ref<MemberPopup> self = this;
    auto name = m_name;
    Session::get().setViewer(m_clientId, viewer, [self, name, viewer](bool ok, std::string err) {
        self->result(ok, err, viewer ? name + " ahora solo puede mirar" : name + " ya puede editar");
    });
}

void MemberPopup::onKick(CCObject*) {
    Ref<MemberPopup> self = this;
    auto name = m_name;
    createQuickPopup("Sacar de la sala",
        fmt::format("¿Sacar a <cr>{}</c> de la sala? Para volver tendrás que invitarlo otra vez.", name),
        "Cancelar", "Sacar",
        [self, name](FLAlertLayer*, bool yes) {
            if (!yes) return;
            Session::get().kick(self->m_clientId, [self, name](bool ok, std::string err) {
                self->result(ok, err, name + " salió de la sala");
            });
        });
}

} // namespace collab
