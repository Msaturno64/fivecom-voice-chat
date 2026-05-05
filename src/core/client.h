#pragma once
#include "audio/wasapi.h"
#include "audio/codec.h"
#include "network/socket.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <string>

namespace fivecom {

struct ClientPeer {
    uint32_t    id = 0;
    std::string nick;
    Endpoint    ep;
    uint32_t    last_seen_ms = 0;
};

class ClientNode {
public:
    // Se conecta a un canal por código (resuelve master_ep vía rendezvous).
    bool start(const std::string& code, const std::string& nick, std::string& err);
    void stop();
    bool running() const { return running_.load(); }

    // Failover: el net_loop pone esto en true cuando la elección lo escoge.
    // El thread principal lo lee y dispara la transición a master.
    bool failover_requested() const { return failover_requested_.load(); }

    // Snapshot del estado para entregárselo al master nuevo.
    std::string                 channel_code()   const { return code_; }
    std::string                 channel_secret() const { return secret_; }
    std::vector<ClientPeer>     snapshot_peers();

private:
    void net_loop();
    void try_election();

    UdpSocket sock_;
    Endpoint  master_ep_;
    Endpoint  public_ep_;
    std::string nick_;
    std::string code_;
    std::string secret_;
    std::atomic<bool> joined_{false};
    std::atomic<bool> failover_requested_{false};
    std::atomic<uint32_t> last_master_seen_ms_{0};
    std::atomic<uint32_t> last_election_ms_{0};

    std::vector<ClientPeer> peers_;
    std::mutex peers_mu_;

    WasapiCapture capture_;
    WasapiRender  render_;

    OpusEncoderW enc_;
    OpusDecoderW dec_;

    JitterBuffer jitter_{2, 6};

    std::thread net_thread_;
    std::atomic<bool> running_{false};
};

} // namespace fivecom
