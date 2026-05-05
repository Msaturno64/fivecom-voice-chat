#pragma once
#include <cstdint>
#include <cstring>

namespace fivecom {

constexpr uint16_t DEFAULT_PORT  = 7777;
constexpr uint32_t MAGIC         = 0x46434F4Du; // "FCOM"
constexpr size_t   MAX_PACKET    = 1500;
constexpr size_t   MAX_NICK      = 31;
constexpr size_t   SECRET_LEN    = 64; // hex string (32 chars) + padding

enum PacketType : uint8_t {
    PKT_JOIN       = 1, // client -> master: announce + nick
    PKT_JOIN_ACK   = 2, // master -> client: assigned id + master nick + secret
    PKT_AUDIO      = 3, // bidirectional: opus payload
    PKT_LEAVE      = 4, // client -> master: graceful disconnect
    PKT_PING       = 5, // keepalive entre cualquier par de peers (mesh)
    PKT_PEER_LIST  = 6, // master -> clients: roster completo con endpoints
    PKT_NEW_MASTER = 7, // ex-cliente recién promovido -> resto: tomó el rol
};

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t magic;     // MAGIC
    uint8_t  type;
    uint32_t peer_id;   // sender id (0 if unassigned)
    uint16_t data_len;  // payload size
};

struct JoinPayload {
    char nick[MAX_NICK + 1];
};

struct JoinAckPayload {
    uint32_t assigned_id;
    char     master_nick[MAX_NICK + 1];
    char     channel_secret[SECRET_LEN]; // para que cualquier cliente pueda promoverse
};

struct PeerListEntry {
    uint32_t peer_id;
    char     nick[MAX_NICK + 1];
    uint32_t pub_ip;   // network byte order (sin_addr.s_addr)
    uint16_t pub_port; // network byte order (sin_port)
};

struct NewMasterPayload {
    uint32_t new_master_id;
    char     new_master_nick[MAX_NICK + 1];
    uint32_t new_master_ip;   // network byte order
    uint16_t new_master_port; // network byte order
    char     channel_secret[SECRET_LEN]; // demuestra autorización
};
#pragma pack(pop)

inline size_t write_header(uint8_t* buf, uint8_t type, uint32_t peer_id, uint16_t data_len) {
    PacketHeader h;
    h.magic    = MAGIC;
    h.type     = type;
    h.peer_id  = peer_id;
    h.data_len = data_len;
    std::memcpy(buf, &h, sizeof(h));
    return sizeof(h);
}

inline bool parse_header(const uint8_t* buf, size_t buf_len, PacketHeader& out) {
    if (buf_len < sizeof(PacketHeader)) return false;
    std::memcpy(&out, buf, sizeof(out));
    if (out.magic != MAGIC) return false;
    if (sizeof(PacketHeader) + out.data_len > buf_len) return false;
    return true;
}

} // namespace fivecom
