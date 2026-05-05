#include "network/stun.h"
#include <ws2tcpip.h>
#include <random>
#include <cstdint>
#include <cstring>

namespace fivecom {

constexpr uint32_t STUN_MAGIC = 0x2112A442u;

std::string resolve_ipv4(const std::string& host) {
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return "";
    char buf[INET_ADDRSTRLEN] = {0};
    auto* sa = reinterpret_cast<sockaddr_in*>(res->ai_addr);
    inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf));
    freeaddrinfo(res);
    return buf;
}

static void put_u16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8); p[3] = (uint8_t)v;
}
static uint16_t get_u16(const uint8_t* p) { return (uint16_t(p[0]) << 8) | p[1]; }
static uint32_t get_u32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16)
         | (uint32_t(p[2]) <<  8) |  uint32_t(p[3]);
}

bool stun_discover(UdpSocket& sock,
                   const std::string& server_host, uint16_t server_port,
                   Endpoint& public_ep, std::string& err) {
    std::string ip = resolve_ipv4(server_host);
    if (ip.empty()) { err = "STUN: DNS falló para " + server_host; return false; }
    Endpoint server = Endpoint::from(ip, server_port);

    // Transaction ID: 12 bytes random
    uint8_t tx[12];
    std::random_device rd;
    for (int i = 0; i < 12; i += 4) {
        uint32_t r = rd();
        std::memcpy(tx + i, &r, 4);
    }

    uint8_t req[20];
    put_u16(req + 0, 0x0001);     // Binding Request
    put_u16(req + 2, 0x0000);     // length 0
    put_u32(req + 4, STUN_MAGIC); // magic cookie
    std::memcpy(req + 8, tx, 12);

    // Hasta 4 reintentos de 500ms (similar a recomendación RFC 5389)
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (sock.send(req, sizeof(req), server) <= 0) {
            err = "STUN: send falló";
            return false;
        }
        // Esperamos respuestas hasta que matchee el transaction id
        // (descartando paquetes ajenos que pudieran llegar).
        for (int i = 0; i < 8; ++i) {
            uint8_t resp[1500];
            Endpoint from;
            int n = sock.recv(resp, sizeof(resp), from, 500);
            if (n < 20) break;
            if (get_u32(resp + 4) != STUN_MAGIC) continue;
            if (std::memcmp(resp + 8, tx, 12) != 0) continue;

            uint16_t type = get_u16(resp);
            // Bits de clase en STUN: C0=0x0010, C1=0x0100. method = type sin esos.
            if ((type & 0xFEEF) != 0x0001) { err = "STUN: tipo inválido"; return false; }
            if ((type & 0x0110) != 0x0100) { err = "STUN: error response"; return false; }

            uint16_t att_len = get_u16(resp + 2);
            if (20 + att_len > n) { err = "STUN: longitud inconsistente"; return false; }

            const uint8_t* p   = resp + 20;
            const uint8_t* end = p + att_len;
            while (p + 4 <= end) {
                uint16_t at = get_u16(p);
                uint16_t al = get_u16(p + 2);
                const uint8_t* av = p + 4;
                if (av + al > end) break;
                if (at == 0x0020 || at == 0x8020) { // XOR-MAPPED-ADDRESS
                    if (al < 8) break;
                    uint8_t fam = av[1];
                    if (fam != 0x01) { err = "STUN: no-IPv4"; return false; }
                    uint16_t xport = (uint16_t(av[2]) << 8) | av[3];
                    uint16_t port  = xport ^ (uint16_t)(STUN_MAGIC >> 16);
                    uint32_t xaddr = get_u32(av + 4);
                    uint32_t addr  = xaddr ^ STUN_MAGIC;

                    auto& a = public_ep.addr;
                    std::memset(&a, 0, sizeof(a));
                    a.sin_family      = AF_INET;
                    a.sin_port        = htons(port);
                    a.sin_addr.s_addr = htonl(addr);
                    return true;
                }
                size_t pad = (4 - (al & 3)) & 3;
                p = av + al + pad;
            }
            err = "STUN: respuesta sin XOR-MAPPED-ADDRESS";
            return false;
        }
    }
    err = "STUN: timeout (¿firewall bloquea UDP saliente?)";
    return false;
}

} // namespace fivecom
