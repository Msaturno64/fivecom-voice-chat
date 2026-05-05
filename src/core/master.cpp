#include "core/master.h"
#include "core/app.h"
#include "core/config.h"
#include "core/log.h"
#include "network/stun.h"

#include <chrono>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace fivecom {

constexpr uint32_t PEER_TIMEOUT_MS = 10000;

static uint32_t now_ms() {
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}

static int16_t clip16(int32_t v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static bool detect_speech(const int16_t* pcm, size_t n) {
    int64_t sumsq = 0;
    for (size_t i = 0; i < n; ++i) sumsq += (int64_t)pcm[i] * pcm[i];
    return std::sqrt((double)sumsq / (double)n) > 500.0;
}

bool MasterNode::start(uint16_t port, const std::string& nick, std::string& err) {
    if (running_.load()) { err = "already running"; return false; }
    nick_ = nick;
    if (nick_.size() > MAX_NICK) nick_.resize(MAX_NICK);
    port_ = port;

    // Bind ephemeral: no necesitamos puerto fijo, el endpoint público lo
    // descubrimos por STUN.
    if (!sock_.bind(0)) { err = "bind() falló"; return false; }

    // STUN: descubrir endpoint público (debe hacerse ANTES de arrancar net_loop
    // porque comparte la misma socket que el audio).
    log_msg("master: STUN discovery...");
    if (!stun_discover(sock_, "stun.l.google.com", 19302, public_ep_, err)) {
        log_msg("master: STUN failed: %s", err.c_str());
        sock_.close();
        return false;
    }
    log_msg("master: STUN ok, public=%s:%u", public_ep_.ip().c_str(), public_ep_.port());

    // Rendezvous: si tenemos un código guardado, intentar reusarlo (refresh
    // con su secret). Si la sala expiró del KV o no tenemos saved → /create.
    rdv_.base_url = load_rendezvous_url();
    if (rdv_.base_url.empty()) {
        err = "Falta rendezvous_url.txt al lado del .exe (ver worker/README.md)";
        sock_.close();
        return false;
    }

    Config cfg;
    config_load(cfg);

    bool reclaimed = false;
    if (!cfg.last_hosted_code.empty() && !cfg.last_hosted_secret.empty()) {
        std::string e2;
        if (rdv_.refresh(cfg.last_hosted_code, cfg.last_hosted_secret,
                         public_ep_, nick_, e2)) {
            code_   = cfg.last_hosted_code;
            secret_ = cfg.last_hosted_secret;
            reclaimed = true;
            log_msg("master: reusando código guardado %s", code_.c_str());
        } else {
            log_msg("master: reclaim falló (%s) → creando nuevo", e2.c_str());
        }
    }
    if (!reclaimed) {
        if (!rdv_.create(public_ep_, nick_, code_, secret_, err)) {
            log_msg("master: rendezvous create failed: %s", err.c_str());
            sock_.close();
            return false;
        }
        log_msg("master: nuevo código=%s", code_.c_str());
    }

    // Persistir nick + nuevo/reusado canal.
    cfg.nick = nick_;
    cfg.last_hosted_code   = code_;
    cfg.last_hosted_secret = secret_;
    config_record_hosted(cfg, code_);
    config_save(cfg);

    AppState::instance().channel_code   = code_;
    AppState::instance().channel_secret = secret_;
    AppState::instance().public_ip      = public_ep_.ip();
    AppState::instance().public_port    = public_ep_.port();
    AppState::instance().self_id.store(1);

    running_.store(true);

    capture_.set_device_id(cfg.capture_device_id);
    render_.set_device_id(cfg.render_device_id);

    bool ok_cap = capture_.start([this](const int16_t* pcm, size_t n) {
        bool speaking = detect_speech(pcm, n);
        AppState::instance().self_speaking.store(speaking);
        if (AppState::instance().muted.load()) {
            int16_t zero[FRAME_SAMPLES] = {0};
            mic_jitter_.push(zero, FRAME_SAMPLES);
            return;
        }
        mic_jitter_.push(pcm, n);
    }, err);
    if (!ok_cap) {
        running_.store(false);
        sock_.close();
        return false;
    }

    bool ok_ren = render_.start([this](int16_t* pcm, size_t n) -> bool {
        if (!running_.load()) return false;
        if (AppState::instance().deafened.load()) {
            std::fill(pcm, pcm + n, 0);
            return true;
        }
        // n == FRAME_SAMPLES siempre (el render lo garantiza)
        local_play_jitter_.pop(pcm);
        return true;
    }, err);
    if (!ok_ren) {
        capture_.stop();
        running_.store(false);
        sock_.close();
        return false;
    }

    net_thread_  = std::thread([this]() { net_loop(); });
    tick_thread_ = std::thread([this]() { tick_loop(); });
    poll_thread_ = std::thread([this]() { poll_loop(); });
    return true;
}

bool MasterNode::start_failover(const std::string& code, const std::string& secret,
                                const std::string& nick,
                                const std::vector<InheritedPeer>& inherited,
                                std::string& err) {
    if (running_.load()) { err = "already running"; return false; }
    nick_ = nick;
    if (nick_.size() > MAX_NICK) nick_.resize(MAX_NICK);

    if (!sock_.bind(0)) { err = "bind() falló"; return false; }

    log_msg("master(failover): STUN...");
    if (!stun_discover(sock_, "stun.l.google.com", 19302, public_ep_, err)) {
        log_msg("master(failover): STUN failed: %s", err.c_str());
        sock_.close();
        return false;
    }
    log_msg("master(failover): public=%s:%u", public_ep_.ip().c_str(), public_ep_.port());

    rdv_.base_url = load_rendezvous_url();
    if (rdv_.base_url.empty()) {
        err = "Falta rendezvous URL"; sock_.close(); return false;
    }
    if (!rdv_.refresh(code, secret, public_ep_, nick_, err)) {
        log_msg("master(failover): refresh falló: %s", err.c_str());
        sock_.close();
        return false;
    }
    code_   = code;
    secret_ = secret;

    AppState::instance().channel_code   = code_;
    AppState::instance().channel_secret = secret_;
    AppState::instance().public_ip      = public_ep_.ip();
    AppState::instance().public_port    = public_ep_.port();
    AppState::instance().self_id.store(1);

    running_.store(true);

    Config cfg;
    config_load(cfg);
    capture_.set_device_id(cfg.capture_device_id);
    render_.set_device_id(cfg.render_device_id);

    // Pre-populate peers_ con la lista heredada (sin contar a uno mismo).
    {
        std::lock_guard<std::mutex> lk(peers_mu_);
        uint32_t max_id = 1;
        for (auto& ip : inherited) {
            if (ip.ep == public_ep_) continue; // saltear self si está en la lista
            auto p = std::make_unique<MasterPeer>();
            p->id   = ip.id;
            p->ep   = ip.ep;
            p->nick = ip.nick;
            p->last_seen_ms = now_ms();
            std::string ig;
            if (!p->dec.init(ig) || !p->enc.init(ig)) continue;
            if (ip.id > max_id) max_id = ip.id;
            peers_.push_back(std::move(p));
        }
        next_id_.store(max_id + 1);
    }

    // Audio: mismo callback que start().
    bool ok_cap = capture_.start([this](const int16_t* pcm, size_t n) {
        bool speaking = detect_speech(pcm, n);
        AppState::instance().self_speaking.store(speaking);
        if (AppState::instance().muted.load()) {
            int16_t zero[FRAME_SAMPLES] = {0};
            mic_jitter_.push(zero, FRAME_SAMPLES);
            return;
        }
        mic_jitter_.push(pcm, n);
    }, err);
    if (!ok_cap) { running_.store(false); sock_.close(); return false; }

    bool ok_ren = render_.start([this](int16_t* pcm, size_t n) -> bool {
        if (!running_.load()) return false;
        if (AppState::instance().deafened.load()) {
            std::fill(pcm, pcm + n, 0);
            return true;
        }
        local_play_jitter_.pop(pcm);
        return true;
    }, err);
    if (!ok_ren) {
        capture_.stop();
        running_.store(false);
        sock_.close();
        return false;
    }

    net_thread_  = std::thread([this]() { net_loop(); });
    tick_thread_ = std::thread([this]() { tick_loop(); });
    poll_thread_ = std::thread([this]() { poll_loop(); });

    // Anunciar a los peers heredados (cada uno actualiza su master_ep_).
    {
        uint8_t buf[MAX_PACKET];
        NewMasterPayload nm{};
        nm.new_master_id = 1;
        std::strncpy(nm.new_master_nick, nick_.c_str(), MAX_NICK);
        nm.new_master_ip   = public_ep_.addr.sin_addr.s_addr;
        nm.new_master_port = public_ep_.addr.sin_port;
        std::strncpy(nm.channel_secret, secret_.c_str(), SECRET_LEN - 1);
        size_t off = write_header(buf, PKT_NEW_MASTER, 1, sizeof(nm));
        std::memcpy(buf + off, &nm, sizeof(nm));

        std::lock_guard<std::mutex> lk(peers_mu_);
        for (auto& p : peers_) {
            // 3 retransmisiones spaced ~50ms para máxima chance de que llegue.
            for (int k = 0; k < 3; ++k) sock_.send(buf, off + sizeof(nm), p->ep);
        }
    }

    log_msg("master(failover): activo, code=%s, %zu peers heredados",
            code_.c_str(), inherited.size());
    return true;
}

void MasterNode::stop() {
    if (!running_.load()) return;
    running_.store(false);
    capture_.stop();
    render_.stop();
    if (net_thread_.joinable())  net_thread_.join();
    if (tick_thread_.joinable()) tick_thread_.join();
    if (poll_thread_.joinable()) poll_thread_.join();
    sock_.close();
    {
        std::lock_guard<std::mutex> lk(peers_mu_);
        peers_.clear();
    }
    mic_jitter_.clear();
    local_play_jitter_.clear();
}

MasterPeer* MasterNode::find_peer(uint32_t id) {
    for (auto& p : peers_) if (p->id == id) return p.get();
    return nullptr;
}

MasterPeer* MasterNode::find_peer_by_ep(const Endpoint& ep) {
    for (auto& p : peers_) if (p->ep == ep) return p.get();
    return nullptr;
}

void MasterNode::publish_peer_list() {
    // Construir bajo el lock; enviar luego (sin lock).
    std::vector<PeerListEntry> entries;
    std::vector<Endpoint> recipients;
    {
        std::lock_guard<std::mutex> lk(peers_mu_);
        entries.reserve(peers_.size() + 1);

        // Master en la lista (con su endpoint público)
        PeerListEntry m{};
        m.peer_id  = 1;
        std::strncpy(m.nick, nick_.c_str(), MAX_NICK);
        m.pub_ip   = public_ep_.addr.sin_addr.s_addr;
        m.pub_port = public_ep_.addr.sin_port;
        entries.push_back(m);

        for (auto& p : peers_) {
            PeerListEntry e{};
            e.peer_id  = p->id;
            std::strncpy(e.nick, p->nick.c_str(), MAX_NICK);
            e.pub_ip   = p->ep.addr.sin_addr.s_addr;
            e.pub_port = p->ep.addr.sin_port;
            entries.push_back(e);
            recipients.push_back(p->ep);
        }
    }

    // Reflejar en GUI local
    {
        std::vector<PeerInfo> view;
        for (auto& e : entries) {
            PeerInfo pi;
            pi.id = e.peer_id;
            pi.nick = e.nick;
            pi.speaking = false;
            view.push_back(pi);
        }
        AppState::instance().set_peers(view);
    }

    if (entries.empty()) return;
    uint8_t buf[MAX_PACKET];
    uint16_t plen = (uint16_t)(entries.size() * sizeof(PeerListEntry));
    if (sizeof(PacketHeader) + plen > sizeof(buf)) return;
    size_t off = write_header(buf, PKT_PEER_LIST, 1, plen);
    std::memcpy(buf + off, entries.data(), plen);
    for (auto& ep : recipients) {
        sock_.send(buf, off + plen, ep);
    }
}

void MasterNode::net_loop() {
    uint8_t buf[MAX_PACKET];
    int16_t pcm[FRAME_SAMPLES];

    while (running_.load()) {
        Endpoint from;
        int n = sock_.recv(buf, sizeof(buf), from, 100);
        if (n <= 0) continue;

        PacketHeader h;
        if (!parse_header(buf, n, h)) continue;
        const uint8_t* payload = buf + sizeof(h);

        switch (h.type) {
            case PKT_JOIN: {
                if (h.data_len < sizeof(JoinPayload)) break;
                JoinPayload jp;
                std::memcpy(&jp, payload, sizeof(jp));
                jp.nick[MAX_NICK] = 0;

                uint32_t assigned = 0;
                {
                    std::lock_guard<std::mutex> lk(peers_mu_);
                    auto* existing = find_peer_by_ep(from);
                    if (existing) {
                        existing->nick = jp.nick;
                        existing->last_seen_ms = now_ms();
                        assigned = existing->id;
                    } else {
                        auto p = std::make_unique<MasterPeer>();
                        p->id = next_id_.fetch_add(1);
                        p->ep = from;
                        p->nick = jp.nick;
                        p->last_seen_ms = now_ms();
                        std::string ig;
                        if (!p->dec.init(ig) || !p->enc.init(ig)) break;
                        assigned = p->id;
                        peers_.push_back(std::move(p));
                    }
                }

                // ACK con secret incluido (para que el cliente pueda
                // promoverse a master si éste cae).
                uint8_t ack[MAX_PACKET];
                JoinAckPayload ap{};
                ap.assigned_id = assigned;
                std::strncpy(ap.master_nick, nick_.c_str(), MAX_NICK);
                std::strncpy(ap.channel_secret, secret_.c_str(), SECRET_LEN - 1);
                size_t off = write_header(ack, PKT_JOIN_ACK, 1, sizeof(ap));
                std::memcpy(ack + off, &ap, sizeof(ap));
                sock_.send(ack, off + sizeof(ap), from);

                publish_peer_list();
                break;
            }
            case PKT_AUDIO: {
                std::lock_guard<std::mutex> lk(peers_mu_);
                auto* p = find_peer_by_ep(from);
                if (!p) break;
                p->last_seen_ms = now_ms();
                int dec = p->dec.decode(payload, h.data_len, pcm);
                if (dec > 0) p->jitter.push(pcm, (size_t)dec);
                break;
            }
            case PKT_PING: {
                std::lock_guard<std::mutex> lk(peers_mu_);
                if (auto* p = find_peer_by_ep(from)) p->last_seen_ms = now_ms();
                break;
            }
            case PKT_LEAVE: {
                bool changed = false;
                {
                    std::lock_guard<std::mutex> lk(peers_mu_);
                    auto it = std::remove_if(peers_.begin(), peers_.end(),
                        [&](const std::unique_ptr<MasterPeer>& x){ return x->ep == from; });
                    if (it != peers_.end()) { peers_.erase(it, peers_.end()); changed = true; }
                }
                if (changed) publish_peer_list();
                break;
            }
            default: break;
        }
    }
}

void MasterNode::tick_loop() {
    using namespace std::chrono;
    auto next_tick = steady_clock::now();
    uint32_t last_cleanup = now_ms();

    while (running_.load()) {
        next_tick += milliseconds(FRAME_MS);
        std::this_thread::sleep_until(next_tick);

        // Frame del mic local del master
        int16_t mic_pcm[FRAME_SAMPLES];
        mic_jitter_.pop(mic_pcm);

        // Frame de cada peer
        std::vector<MasterPeer*> alive;
        {
            std::lock_guard<std::mutex> lk(peers_mu_);
            alive.reserve(peers_.size());
            for (auto& p : peers_) {
                p->jitter.pop(p->cur_pcm);
                p->has_frame = true;
                p->speaking = detect_speech(p->cur_pcm, FRAME_SAMPLES);
                alive.push_back(p.get());
            }
        }

        // Mix-minus por peer: master_mic + sum(otros peers)
        for (auto* target : alive) {
            int32_t mix[FRAME_SAMPLES] = {0};
            for (size_t i = 0; i < FRAME_SAMPLES; ++i) mix[i] += mic_pcm[i];
            for (auto* other : alive) {
                if (other == target) continue;
                for (size_t i = 0; i < FRAME_SAMPLES; ++i) mix[i] += other->cur_pcm[i];
            }
            int16_t out_pcm[FRAME_SAMPLES];
            for (size_t i = 0; i < FRAME_SAMPLES; ++i) out_pcm[i] = clip16(mix[i]);

            uint8_t buf[MAX_PACKET];
            size_t off = write_header(buf, PKT_AUDIO, 1, 0);
            int n_enc = target->enc.encode(out_pcm, buf + off, MAX_PACKET - off);
            if (n_enc > 2) {
                PacketHeader h;
                std::memcpy(&h, buf, sizeof(h));
                h.data_len = (uint16_t)n_enc;
                std::memcpy(buf, &h, sizeof(h));
                sock_.send(buf, off + n_enc, target->ep);
            }
        }

        // Mix local (parlantes del master): sum de peers (sin mic)
        int32_t lmix[FRAME_SAMPLES] = {0};
        for (auto* p : alive) {
            for (size_t i = 0; i < FRAME_SAMPLES; ++i) lmix[i] += p->cur_pcm[i];
        }
        int16_t lout[FRAME_SAMPLES];
        for (size_t i = 0; i < FRAME_SAMPLES; ++i) lout[i] = clip16(lmix[i]);
        local_play_jitter_.push(lout, FRAME_SAMPLES);

        // Cleanup peers timed-out cada 1s
        if (now_ms() - last_cleanup > 1000) {
            last_cleanup = now_ms();
            uint32_t cutoff = now_ms() - PEER_TIMEOUT_MS;
            bool changed = false;
            {
                std::lock_guard<std::mutex> lk(peers_mu_);
                auto it = std::remove_if(peers_.begin(), peers_.end(),
                    [&](const std::unique_ptr<MasterPeer>& x){
                        return (int32_t)(x->last_seen_ms - cutoff) < 0;
                    });
                if (it != peers_.end()) {
                    peers_.erase(it, peers_.end());
                    changed = true;
                }
            }
            if (changed) publish_peer_list();
        }
    }
}

void MasterNode::poll_loop() {
    using namespace std::chrono;
    uint32_t last_refresh = now_ms();

    while (running_.load()) {
        // Dormir 1s en pasos chicos para responder rápido a stop().
        for (int i = 0; i < 10 && running_.load(); ++i) {
            std::this_thread::sleep_for(milliseconds(100));
        }
        if (!running_.load()) break;

        // 1. Pedir lista de clientes nuevos al rendezvous.
        std::vector<RemotePeer> pending;
        std::string err;
        if (!rdv_.poll(code_, secret_, pending, err)) {
            log_msg("master: poll error: %s", err.c_str());
        } else if (!pending.empty()) {
            std::lock_guard<std::mutex> lk(punch_mu_);
            uint32_t t = now_ms();
            for (auto& rp : pending) {
                Endpoint e = Endpoint::from(rp.ip, rp.port);
                bool found = false;
                for (auto& pt : punch_targets_) {
                    if (pt.ep == e) { pt.added_ms = t; found = true; break; }
                }
                if (!found) {
                    punch_targets_.push_back({e, t});
                    log_msg("master: nuevo punch target %s:%u (nick=%s)",
                            rp.ip.c_str(), rp.port, rp.nick.c_str());
                }
            }
        }

        // 2. Mandar probes a los punch_targets (abre el mapping NAT del master
        //    hacia el endpoint del cliente, así su PKT_JOIN puede entrar).
        std::vector<Endpoint> probes;
        {
            std::lock_guard<std::mutex> lk(punch_mu_);
            uint32_t t = now_ms();
            // Sacar targets viejos (> 20s).
            auto it = std::remove_if(punch_targets_.begin(), punch_targets_.end(),
                [&](const PunchTarget& pt){ return (t - pt.added_ms) > 20000; });
            punch_targets_.erase(it, punch_targets_.end());
            for (auto& pt : punch_targets_) probes.push_back(pt.ep);
        }
        // No mandar a los que ya son peers (ya están conectados).
        if (!probes.empty()) {
            std::lock_guard<std::mutex> lk(peers_mu_);
            probes.erase(std::remove_if(probes.begin(), probes.end(),
                [&](const Endpoint& e){
                    for (auto& p : peers_) if (p->ep == e) return true;
                    return false;
                }), probes.end());
        }
        if (!probes.empty()) {
            uint8_t pb[64];
            size_t off = write_header(pb, PKT_PING, 1, 0);
            for (auto& e : probes) {
                // 5 paquetes spaced ~50ms para abrir el mapping.
                for (int k = 0; k < 5; ++k) {
                    sock_.send(pb, off, e);
                    std::this_thread::sleep_for(milliseconds(50));
                    if (!running_.load()) return;
                }
            }
        }

        // 3. Cada 10 min refrescar endpoint+nick en el rendezvous. Sin esto la
        //    sala vive 30 días igual (TTL de KV); refresh sólo es útil si el
        //    NAT mapping rotó (raro mientras hay tráfico) o el nick cambió.
        //    Frecuencia baja para no romper el límite free de KV writes.
        if (now_ms() - last_refresh > 10 * 60 * 1000) {
            last_refresh = now_ms();
            std::string e2;
            rdv_.refresh(code_, secret_, public_ep_, nick_, e2);
        }
    }
}

} // namespace fivecom
