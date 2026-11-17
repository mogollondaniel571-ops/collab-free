// Importador de Mods: un botón en el menú principal para instalar archivos .geode rápido.

#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/binding/CCMenuItemSpriteExtra.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/file.hpp>
#include <asp/fs/fs.hpp>

using namespace geode::prelude;

static void importMods() {
    async::spawn(
        file::pickMany(file::FilePickOptions {
            .filters = { file::FilePickOptions::Filter {
                .description = "Mods de Geode",
                .files = { "*.geode" },
            }},
        }),
        [](file::PickManyResult result) {
            if (!result.isOk()) {
                FLAlertLayer::create("Importar mods", "No se pudo abrir la ventana para elegir archivos.", "OK")->show();
                return;
            }
            auto paths = std::move(result).unwrap();
            if (paths.empty()) return; // cancelaste

            std::vector<std::string> ok;
            std::vector<std::string> failed;
            auto modsDir = dirs::getModsDir();

            for (auto const& path : paths) {
                auto name = path.filename();
                if (!name) continue;
                std::string fileName = std::string(name->string());

                if (fileName.size() < 6 || fileName.substr(fileName.size() - 6) != ".geode") {
                    failed.push_back(fileName + " (no es un .geode)");
                    continue;
                }

                auto copied = asp::fs::copy(path, modsDir / fileName);
                if (copied) {
                    ok.push_back(fileName);
                    log::info("Importado: {}", fileName);
                } else {
                    failed.push_back(fileName);
                    log::warn("No se pudo importar {}", fileName);
                }
            }

            std::string msg;
            if (!ok.empty()) {
                msg += fmt::format("<cg>Instalados ({}):</c>\n", ok.size());
                for (auto const& n : ok) msg += n + "\n";
            }
            if (!failed.empty()) {
                msg += fmt::format("<cr>No se pudieron instalar ({}):</c>\n", failed.size());
                for (auto const& n : failed) msg += n + "\n";
            }

            if (ok.empty()) {
                FLAlertLayer::create("Importar mods", msg, "OK")->show();
                return;
            }

            msg += "\nHay que <cy>reiniciar el juego</c> para activarlos.";
            createQuickPopup(
                "Importar mods", msg, "Después", "Reiniciar",
                [](FLAlertLayer*, bool restart) {
                    if (restart) game::restart(true);
                }
            );
        }
    );
}

class $modify(ImporterMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        auto spr = CircleButtonSprite::createWithSpriteFrameName(
            "GJ_downloadsIcon_001.png", 1.f, CircleBaseColor::Green, CircleBaseSize::MediumAlt);
        if (!spr) return true;

        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(ImporterMenuLayer::onImportMods));
        btn->setID("import-mods-button"_spr);

        if (auto bottomMenu = this->getChildByID("bottom-menu")) {
            bottomMenu->addChild(btn);
            bottomMenu->updateLayout();
        } else {
            // por si el menú no tiene IDs: lo ponemos abajo a la izquierda
            auto winSize = CCDirector::get()->getWinSize();
            auto menu = CCMenu::create();
            menu->setPosition({0, 0});
            btn->setPosition({winSize.width / 2 - 140.f, 45.f});
            menu->addChild(btn);
            this->addChild(menu, 10);
        }
        return true;
    }

    void onImportMods(CCObject*) {
        importMods();
    }
};
