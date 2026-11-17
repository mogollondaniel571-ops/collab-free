#pragma once

// Conexión con el servidor de salas (HTTP + long polling).

#include <Geode/Geode.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>
#include <matjson.hpp>
#include <functional>
#include <string>

namespace collab {

using JsonResult = geode::Result<matjson::Value, std::string>;
using JsonCallback = std::function<void(JsonResult)>;

// Ayudas para leer JSON sin que truene si falta algo
std::string jstr(matjson::Value const& v, std::string_view key, std::string def = "");
double jnum(matjson::Value const& v, std::string_view key, double def = 0);
std::vector<matjson::Value> jarr(matjson::Value const& v, std::string_view key);

class Net {
public:
    enum class State { Off, Connecting, Ready };

    static Net& get();

    /// Conecta (si no lo está) y empieza a escuchar eventos.
    void start();
    /// Se llama cada frame desde el Ticker.
    void tick(float dt);

    bool isReady() const { return m_state == State::Ready; }
    State state() const { return m_state; }
    std::string const& clientId() const { return m_clientId; }
    std::string const& lastError() const { return m_lastError; }
    std::string serverUrl() const;

    /// Llama a una ruta del servidor. El callback corre en el hilo principal.
    void call(std::string const& path, matjson::Value body, JsonCallback cb, int timeoutSecs = 60);

    /// Lo que hay que hacer con cada evento que llega (lo pone Session).
    std::function<void(matjson::Value const&)> onEvent;
    /// Se llama cada vez que se conecta (reconnect = ya estaba conectado antes).
    std::function<void(matjson::Value const&, bool)> onHello;

private:
    void hello();
    void poll();
    void scheduleRetry(float seconds);

    State m_state = State::Off;
    std::string m_clientId;
    std::string m_token;
    std::string m_lastError;
    double m_since = 0;
    bool m_polling = false;
    bool m_wasConnected = false;
    float m_retryIn = -1.f;
    float m_backoff = 2.f;
    uint64_t m_generation = 0; // cambia cada vez que se reconecta
    arc::TaskHandle<void> m_pollTask;
};

} // namespace collab
