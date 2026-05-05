#include "core/client.h"
#include "core/app.h"
#include "core/config.h"
#include "core/log.h"
#include "network/packet.h"
#include "network/stun.h"
#include "network/rendezvous.h"

#include <cstring>
#include <cmath>
#include <vector>
#include <chrono>
#include <algorithm>

namespace fivecom {

static uint32_t now_ms() {
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}

static bool detect_speech(const int16_t* pcm, size_t n) {
    int64_t sumsq = 0;
    for (size_t i = 0; i < n; ++i) sumsq += (int64_t)pcm[i] * pcm[i];
    double rms = std::sqrt((double)sumsq / (double)n);
    return rms > 500.0; // umbral simple
}

bool ClientNode::start(const std::string& code, const std::string& nick, std::string& err) {
    if (running_.load()) { err = "already running"; return false; }
    nick_ = nick;
    if (nick_.size() > MAX_NICK) nick_.resize(MAX_NICK);

    if (!sock_.bind(0)) { err = "bind() falló"; return false; }

    // STUN: descubrir endpoint público del cliente para registrarlo en el rdv.
    log_msg("client: STUN discovery...");
    if (!stun_discover(sock_, "stun.l.google.com", 19302, public_ep_, err)) {
        log_msg("client: STUN failed: %s", err.c_str());
        sock_.close();
        return false;
    }
    log_msg("client: STUN ok, public=%s:%u", public_ep_.ip().c_str(), public_ep_.port());

    // Rendezvous: lookup del master por código + registrar nuestro endpoint.
    Rendezvous rdv;
    rdv.base_url = load_rendezvous_url();
    if (rdv.base_url.empty()) {
        err = "Falta rendezvous_url.txt al lado del .exe";
        sock_.close();
        return false;
    }
    std::string master_nick;
    if (!rdv.join(code, public_ep_, nick_, master_ep_, master_nick, err)) {
        log_msg("client: rendezvous join failed: %s", err.c_str());
        sock_.close();
        return false;
    }
    log_msg("client: master endpoint=%s:%u (nick=%s)",
            master_ep_.ip().c_str(), master_ep_.port(), master_nick.c_str());

    code_ = code;
    AppState::instance().channel_code = code;
    AppState::instance().master_ip    = master_ep_.ip();
    AppState::instance().public_ip    = public_ep_.ip();
    AppState::instance().public_port  = public_ep_.port();

    // Persistir nick + entrada en el historial.
    {
        Config cfg;
        config_load(cfg);
        cfg.nick = nick_;
        config_record_joined(cfg, code, master_nick);
        config_save(cfg);
    }

    if (!enc_.init(err)) { sock_.close(); return false; }
    if (!dec_.init(err)) { sock_.close(); return false; }

    joined_.store(false);

    // JOIN inicial (el net_loop retransmite cada 250ms hasta recibir JOIN_ACK).
    {
        uint8_t buf[MAX_PACKET];
        JoinPayload jp{};
        std::strncpy(jp.nick, nick_.c_str(), MAX_NICK);
        size_t off = write_header(buf, PKT_JOIN, 0, sizeof(jp));
        std::memcpy(buf + off, &jp, sizeof(jp));
        sock_.send(buf, off + sizeof(jp), master_ep_);
    }

    running_.store(true);

    Config cfg;
    config_load(cfg);
    capture_.set_device_id(cfg.capture_device_id);
    render_.set_device_id(cfg.render_device_id);

    // Captura de micrófono -> Opus -> envío
    bool ok_cap = capture_.start([this](const int16_t* pcm, size_t n) {
        bool speaking = detect_speech(pcm, n);
        AppState::instance().self_speaking.store(speaking);
        if (AppState::instance().muted.load()) return;
        uint8_t buf[MAX_PACKET];
        size_t off = write_header(buf, PKT_AUDIO,
                                  AppState::instance().self_id.load(), 0);
        int n_enc = enc_.encode(pcm, buf + off, MAX_PACKET - off);
        if (n_enc <= 2) return; // DTX/silencio: omitir envío
        // re-escribir data_len
        PacketHeader h;
        std::memcpy(&h, buf, sizeof(h));
        h.data_len = (uint16_t)n_enc;
        std::memcpy(buf, &h, sizeof(h));
        sock_.send(buf, off + n_enc, master_ep_);
    }, err);
    if (!ok_cap) {
        running_.store(false);
        sock_.close();
        return false;
    }

    // Render: jitter -> Opus PLC ya viene aplicado en push() del net_loop
    bool ok_ren = render_.start([this](int16_t* pcm, size_t n) -> bool {
        if (!running_.load()) return false;
        if (AppState::instance().deafened.load()) {
            std::fill(pcm, pcm + n, 0);
            return true;
        }
        // n == FRAME_SAMPLES siempre (el render lo garantiza)
        jitter_.pop(pcm);
        return true;
    }, err);
    if (!ok_ren) {
        capture_.stop();
        running_.store(false);
        sock_.close();
        return false;
    }

    net_thread_ = std::thread([this]() { net_loop(); });
    return true;
}

void ClientNode::stop() {
    if (!running_.load()) return;

    // LEAVE best-effort
    uint8_t buf[64];
    size_t off = write_header(buf, PKT_LEAVE, AppState::instance().self_id.load(), 0);
    sock_.send(buf, off, master_ep_);

    running_.store(false);
    capture_.stop();
    render_.stop();
    if (net_thread_.joinable()) net_thread_.join();
    sock_.close();
    jitter_.clear();
}

void ClientNode::net_loop() {
    uint8_t buf[MAX_PACKET];
    int16_t pcm[FRAME_SAMPLES];
    uint32_t last_ping = now_ms();
    uint32_t last_join = now_ms();
    uint32_t last_mesh = now_ms();
    last_master_seen_ms_.store(now_ms());

    while (running_.load() && !failover_requested_.load()) {
        Endpoint from;
        int n = sock_.recv(buf, sizeof(buf), from, 100);

        // Re-JOIN si todavía no llegó JOIN_ACK.
        if (!joined_.load() && now_ms() - last_join > 250) {
            uint8_t jbuf[MAX_PACKET];
            JoinPayload jp{};
            std::strncpy(jp.nick, nick_.c_str(), MAX_NICK);
            size_t off = write_header(jbuf, PKT_JOIN, 0, sizeof(jp));
            std::memcpy(jbuf + off, &jp, sizeof(jp));
            sock_.send(jbuf, off + sizeof(jp), master_ep_);
            last_join = now_ms();
        }

        // Ping al master cada 2s (keepalive NAT + signal de vida).
        if (now_ms() - last_ping > 2000) {
            uint8_t pbuf[64];
            size_t off = write_header(pbuf, PKT_PING,
                                      AppState::instance().self_id.load(), 0);
            sock_.send(pbuf, off, master_ep_);
            last_ping = now_ms();
        }

        // Mesh keepalive: ping a TODOS los otros peers cada 3s para mantener
        // mappings NAT abiertos (necesario si alguien tiene que pivotear como
        // master nuevo).
        if (now_ms() - last_mesh > 3000) {
            last_mesh = now_ms();
            uint8_t pbuf[64];
            size_t off = write_header(pbuf, PKT_PING,
                                      AppState::instance().self_id.load(), 0);
            uint32_t my_id = AppState::instance().self_id.load();
            std::lock_guard<std::mutex> lk(peers_mu_);
            for (auto& p : peers_) {
                if (p.id == my_id) continue;          // soy yo
                if (p.ep == master_ep_) continue;     // ya recibe ping aparte
                sock_.send(pbuf, off, p.ep);
            }
        }

        // Watchdog del master: si pasaron >5s sin oír nada del master, elección.
        if (joined_.load() && now_ms() - last_master_seen_ms_.load() > 5000) {
            try_election();
        }

        if (n <= 0) continue;

        // Marcar last_seen del peer que nos mandó (para que la elección sepa
        // quién está vivo).
        if (from == master_ep_) {
            last_master_seen_ms_.store(now_ms());
        }
        {
            std::lock_guard<std::mutex> lk(peers_mu_);
            for (auto& p : peers_) {
                if (p.ep == from) { p.last_seen_ms = now_ms(); break; }
            }
        }

        PacketHeader h;
        if (!parse_header(buf, n, h)) continue;
        const uint8_t* payload = buf + sizeof(h);

        switch (h.type) {
            case PKT_JOIN_ACK: {
                if (h.data_len < sizeof(JoinAckPayload)) break;
                JoinAckPayload ack;
                std::memcpy(&ack, payload, sizeof(ack));
                ack.master_nick[MAX_NICK] = 0;
                ack.channel_secret[SECRET_LEN - 1] = 0;
                AppState::instance().self_id.store(ack.assigned_id);
                secret_ = ack.channel_secret;
                joined_.store(true);
                log_msg("client: JOIN_ACK id=%u secret_len=%zu",
                        ack.assigned_id, secret_.size());
                break;
            }
            case PKT_AUDIO: {
                int dec = dec_.decode(payload, h.data_len, pcm);
                if (dec > 0) jitter_.push(pcm, (size_t)dec);
                break;
            }
            case PKT_PEER_LIST: {
                size_t count = h.data_len / sizeof(PeerListEntry);
                std::vector<PeerInfo>   ui_peers;
                std::vector<ClientPeer> new_peers;
                ui_peers.reserve(count);
                new_peers.reserve(count);
                uint32_t now = now_ms();
                for (size_t i = 0; i < count; ++i) {
                    PeerListEntry e;
                    std::memcpy(&e, payload + i * sizeof(e), sizeof(e));
                    e.nick[MAX_NICK] = 0;

                    PeerInfo ui;
                    ui.id = e.peer_id;
                    ui.nick = e.nick;
                    ui.speaking = false;
                    ui_peers.push_back(ui);

                    ClientPeer cp;
                    cp.id = e.peer_id;
                    cp.nick = e.nick;
                    cp.ep.addr.sin_family = AF_INET;
                    cp.ep.addr.sin_addr.s_addr = e.pub_ip;
                    cp.ep.addr.sin_port = e.pub_port;
                    // Conservar last_seen previo si ya lo conocíamos.
                    cp.last_seen_ms = (e.peer_id == AppState::instance().self_id.load())
                                      ? now : 0;
                    new_peers.push_back(cp);
                }
                AppState::instance().set_peers(ui_peers);
                {
                    std::lock_guard<std::mutex> lk(peers_mu_);
                    // Preservar last_seen anterior por id.
                    for (auto& np : new_peers) {
                        for (auto& op : peers_) {
                            if (op.id == np.id && op.last_seen_ms > np.last_seen_ms) {
                                np.last_seen_ms = op.last_seen_ms;
                                break;
                            }
                        }
                    }
                    peers_ = std::move(new_peers);
                }
                break;
            }
            case PKT_NEW_MASTER: {
                if (h.data_len < sizeof(NewMasterPayload)) break;
                NewMasterPayload nm;
                std::memcpy(&nm, payload, sizeof(nm));
                nm.new_master_nick[MAX_NICK] = 0;
                nm.channel_secret[SECRET_LEN - 1] = 0;
                if (secret_ != nm.channel_secret) {
                    log_msg("client: PKT_NEW_MASTER rechazado (secret no coincide)");
                    break;
                }
                Endpoint new_ep;
                std::memset(&new_ep.addr, 0, sizeof(new_ep.addr));
                new_ep.addr.sin_family = AF_INET;
                new_ep.addr.sin_addr.s_addr = nm.new_master_ip;
                new_ep.addr.sin_port = nm.new_master_port;
                master_ep_ = new_ep;
                AppState::instance().master_ip = master_ep_.ip();
                last_master_seen_ms_.store(now_ms());
                joined_.store(false);
                last_join = now_ms() - 250; // dispara JOIN inmediato
                log_msg("client: nuevo master %s -> %s:%u (id=%u)",
                        nm.new_master_nick, master_ep_.ip().c_str(),
                        master_ep_.port(), nm.new_master_id);
                break;
            }
            default: break;
        }
    }
}

void ClientNode::try_election() {
    uint32_t now = now_ms();
    if (now - last_election_ms_.load() < 1000) return;
    last_election_ms_.store(now);

    uint32_t my_id = AppState::instance().self_id.load();
    if (my_id == 0) return;

    std::lock_guard<std::mutex> lk(peers_mu_);
    if (peers_.empty()) return;

    // Buscar el peer con menor id entre los activos (último visto < 8s),
    // excluyendo al master actual (que está silencioso).
    uint32_t lowest = UINT32_MAX;
    for (auto& p : peers_) {
        if (p.ep == master_ep_) continue;          // master muerto
        bool active = (p.id == my_id) || (now - p.last_seen_ms < 8000);
        if (active && p.id < lowest) lowest = p.id;
    }
    if (lowest == UINT32_MAX) return;
    if (lowest == my_id) {
        log_msg("client: elección -> SOY YO (id=%u). Promoviendo a master.", my_id);
        failover_requested_.store(true);
    } else {
        log_msg("client: elección -> peer %u, esperando PKT_NEW_MASTER", lowest);
    }
}

std::vector<ClientPeer> ClientNode::snapshot_peers() {
    std::lock_guard<std::mutex> lk(peers_mu_);
    return peers_;
}

} // namespace fivecom
