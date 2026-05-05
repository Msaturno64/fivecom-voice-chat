#pragma once
#include "audio/wasapi.h"
#include "audio/codec.h"
#include "network/socket.h"
#include "network/packet.h"
#include "network/rendezvous.h"

#include <atomic>
#include <thread>
#include <vector>
#include <memory>
#include <mutex>
#include <string>

namespace fivecom {

struct MasterPeer {
    uint32_t   id;
    Endpoint   ep;
    std::string nick;
    OpusEncoderW enc;
    OpusDecoderW dec;
    JitterBuffer jitter{2, 6};
    int16_t    cur_pcm[FRAME_SAMPLES];
    bool       has_frame = false;
    uint32_t   last_seen_ms = 0;
    bool       speaking = false;

    MasterPeer() = default;
    MasterPeer(const MasterPeer&) = delete;
    MasterPeer& operator=(const MasterPeer&) = delete;
};

struct InheritedPeer {
    uint32_t    id;
    std::string nick;
    Endpoint    ep;
};

class MasterNode {
public:
    bool start(uint16_t port, const std::string& nick, std::string& err);
    // Promoción por failover: usa el code+secret heredados del cliente, hace
    // STUN+/refresh, y pre-popula peers_ con los endpoints conocidos.
    bool start_failover(const std::string& code, const std::string& secret,
                        const std::string& nick,
                        const std::vector<InheritedPeer>& inherited,
                        std::string& err);
    void stop();
    bool running() const { return running_.load(); }

private:
    void net_loop();
    void tick_loop();
    void poll_loop();

    void publish_peer_list();
    MasterPeer* find_peer(uint32_t id);
    MasterPeer* find_peer_by_ep(const Endpoint& ep);

    UdpSocket sock_;
    uint16_t  port_ = 0;
    std::string nick_;

    Rendezvous rdv_;
    std::string code_;
    std::string secret_;
    Endpoint    public_ep_;

    struct PunchTarget { Endpoint ep; uint32_t added_ms; };
    std::vector<PunchTarget> punch_targets_;
    std::mutex punch_mu_;
    std::thread poll_thread_;

    WasapiCapture capture_;
    WasapiRender  render_;

    JitterBuffer mic_jitter_{2, 6};   // mic local del master
    JitterBuffer local_play_jitter_{2, 6}; // mix de peers para parlantes locales

    std::vector<std::unique_ptr<MasterPeer>> peers_;
    std::mutex peers_mu_;
    std::atomic<uint32_t> next_id_{2}; // master = 1

    std::thread net_thread_;
    std::thread tick_thread_;
    std::atomic<bool> running_{false};
};

} // namespace fivecom
