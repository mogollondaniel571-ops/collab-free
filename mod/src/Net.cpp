#include "Net.hpp"
#include <Geode/utils/random.hpp>

#include <Geode/binding/GJAccountManager.hpp>

using namespace geode::prelude;

namespace collab {

std::string jstr(matjson::Value const& v, std::string_view key, std::string def) {
    if (!v.isObject() || !v.contains(key)) return def;
    auto const& x = v[key];
    if (x.isString()) return x.asString().unwrapOr(def);
    if (x.isNumber()) return fmt::format("{}", x.asDouble().unwrapOr(0));
    return def;
}

double jnum(matjson::Value const& v, std::string_view key, double def) {
    if (!v.isObject() || !v.contains(key)) return def;
    return v[key].asDouble().unwrapOr(def);
}

std::vector<matjson::Value> jarr(matjson::Value const& v, std::string_view key) {
    if (!v.isObject() || !v.contains(key)) return {};
    auto const& x = v[key];
    if (!x.isArray()) return {};
    return x.asArray().unwrap();
}

static JsonResult parseResponse(web::WebResponse const& res) {
    auto json = res.json();
    if (res.ok()) {
        if (json) return Ok(json.unwrap());
        return Err(std::string("El servidor respondió algo raro."));
    }
    if (res.code() <= 0) {
        return Err(std::string("No se pudo conectar con el servidor."));
    }
    if (json) {
        auto msg = jstr(json.unwrap(), "error");
        if (!msg.empty()) return Err(msg);
    }
    return Err(fmt::format("Error del servidor ({}).", res.code()));
}

Net& Net::get() {
    static Net instance;
    return instance;
}

std::string Net::serverUrl() const {
    auto url = Mod::get()->getSettingValue<std::string>("server-url");
    while (!url.empty() && (url.back() == '/' || url.back() == ' ')) url.pop_back();
    while (!url.empty() && url.front() == ' ') url.erase(url.begin());
    if (!url.empty() && url.find("://") == std::string::npos) url = "https://" + url;
    return url;
}

void Net::start() {
    if (m_state != State::Off) return;
    m_retryIn = -1.f;
    this->hello();
}

void Net::scheduleRetry(float seconds) {
    m_retryIn = seconds;
}

void Net::tick(float dt) {
    if (m_retryIn >= 0.f) {
        m_retryIn -= dt;
        if (m_retryIn < 0.f) {
            m_retryIn = -1.f;
            if (m_state == State::Off) this->hello();
            else if (m_state == State::Ready && !m_polling) this->poll();
        }
    }
}

void Net::hello() {
    // cualquier escucha vieja queda descartada
    ++m_generation;
    if (m_pollTask) m_pollTask.abort();
    m_polling = false;

    auto am = GJAccountManager::sharedState();
    if (!am || am->m_accountID <= 0) {
        m_lastError = "Inicia sesión en tu cuenta de Geometry Dash para usar Collab.";
        m_state = State::Off;
        this->scheduleRetry(10.f);
        return;
    }
    auto url = this->serverUrl();
    if (url.empty() || url.find("TU-SERVIDOR") != std::string::npos) {
        m_lastError = "Falta poner la dirección del servidor en los ajustes del mod.";
        m_state = State::Off;
        this->scheduleRetry(10.f);
        return;
    }

    m_state = State::Connecting;
    auto body = matjson::Value::object();
    body["accountID"] = am->m_accountID;
    body["username"] = std::string(am->m_username);
    // llave secreta de este juego: protege tu cuenta para que nadie más la use
    auto key = Mod::get()->getSavedValue<std::string>("account-key");
    if (key.size() < 32) {
        static const char* hex = "0123456789abcdef";
        key.clear();
        for (int i = 0; i < 40; i++) key += hex[geode::utils::random::generate<int>(0, 15)];
        Mod::get()->setSavedValue<std::string>("account-key", key);
    }
    body["key"] = key;

    auto req = web::WebRequest();
    req.bodyJSON(body);
    // el servidor gratis puede tardar 1 o 2 minutos en "despertar"
    req.timeout(asp::Duration::fromSecs(150));

    auto gen = m_generation;
    async::spawn(req.post(url + "/hello"), [this, gen](web::WebResponse res) {
        if (gen != m_generation) return;
        auto result = parseResponse(res);
        if (!result) {
            m_lastError = result.unwrapErr();
            m_state = State::Off;
            log::warn("Collab: no se pudo conectar: {}", m_lastError);
            this->scheduleRetry(m_backoff);
            m_backoff = std::min(m_backoff * 2.f, 30.f);
            return;
        }
        auto data = result.unwrap();
        m_clientId = jstr(data, "client");
        m_token = jstr(data, "token");
        m_since = 0;
        m_backoff = 2.f;
        m_lastError.clear();
        m_state = State::Ready;
        log::info("Collab: conectado como {}", m_clientId);
        bool reconnect = m_wasConnected;
        m_wasConnected = true;
        this->poll();
        if (onHello) onHello(data, reconnect);
    });
}

void Net::poll() {
    if (m_state != State::Ready || m_polling) return;
    m_polling = true;

    auto body = matjson::Value::object();
    body["client"] = m_clientId;
    body["token"] = m_token;
    body["since"] = m_since;

    auto req = web::WebRequest();
    req.bodyJSON(body);
    req.timeout(asp::Duration::fromSecs(45));

    auto gen = m_generation;
    m_pollTask = async::spawn(req.post(this->serverUrl() + "/poll"), [this, gen](web::WebResponse res) {
        if (gen != m_generation) return;
        m_polling = false;

        if (res.code() == 401) {
            // el servidor se reinició o nos olvidó: volver a saludar
            m_state = State::Off;
            this->hello();
            return;
        }
        auto result = parseResponse(res);
        if (!result) {
            m_lastError = result.unwrapErr();
            this->scheduleRetry(3.f);
            return;
        }
        auto data = result.unwrap();
        auto events = jarr(data, "events");
        // primero se avanza "since" y se vuelve a escuchar, luego se procesan
        for (auto const& ev : events) {
            m_since = std::max(m_since, jnum(ev, "seq"));
        }
        this->poll();
        for (auto const& ev : events) {
            if (onEvent) onEvent(ev);
        }
    });
}

void Net::call(std::string const& path, matjson::Value body, JsonCallback cb, int timeoutSecs) {
    if (m_state != State::Ready) {
        if (cb) cb(Err(m_lastError.empty() ? std::string("Todavía conectando con el servidor...") : m_lastError));
        return;
    }
    body["client"] = m_clientId;
    body["token"] = m_token;

    auto req = web::WebRequest();
    req.bodyJSON(body);
    req.timeout(asp::Duration::fromSecs(timeoutSecs));

    auto gen = m_generation;
    async::spawn(req.post(this->serverUrl() + path), [this, gen, cb = std::move(cb)](web::WebResponse res) {
        if (res.code() == 401 && gen == m_generation) {
            m_state = State::Off;
            this->hello();
        }
        if (cb) cb(parseResponse(res));
    });
}

} // namespace collab
