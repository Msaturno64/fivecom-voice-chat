#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>

namespace fivecom {

enum class Mode { Idle, Master, Client };

struct PeerInfo {
    uint32_t id;
    std::string nick;
    bool        speaking; // estimación de actividad
};

class AppState {
public:
    static AppState& instance() { static AppState s; return s; }

    std::atomic<Mode>  mode{Mode::Idle};
    std::atomic<bool>  muted{false};
    std::atomic<bool>  deafened{false};
    std::atomic<bool>  self_speaking{false};
    std::atomic<uint32_t> self_id{0};
    std::string nick = "user";
    std::string master_ip;       // sólo en modo Cliente
    std::string local_ip;        // descubierto al iniciar Master
    std::string channel_code;    // código del canal (master: generado; client: ingresado)
    std::string channel_secret;  // sólo master: secret para /poll y /refresh
    std::string public_ip;       // IP pública vista por STUN (ambos modos)
    uint16_t    public_port = 0;

    void set_peers(std::vector<PeerInfo> v) {
        std::lock_guard<std::mutex> lk(mu_);
        peers_ = std::move(v);
    }
    std::vector<PeerInfo> get_peers() {
        std::lock_guard<std::mutex> lk(mu_);
        return peers_;
    }

private:
    std::mutex mu_;
    std::vector<PeerInfo> peers_;
};

} // namespace fivecom
