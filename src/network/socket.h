#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdint>
#include <cstring>
#include <string>

namespace fivecom {

struct Endpoint {
    sockaddr_in addr;

    Endpoint() { std::memset(&addr, 0, sizeof(addr)); }
    Endpoint(const sockaddr_in& a) : addr(a) {}

    static Endpoint from(const std::string& ip, uint16_t port);

    std::string ip() const;
    uint16_t    port() const { return ntohs(addr.sin_port); }

    bool operator==(const Endpoint& o) const {
        return addr.sin_addr.s_addr == o.addr.sin_addr.s_addr
            && addr.sin_port == o.addr.sin_port;
    }
};

class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // bind to 0.0.0.0:port (port=0 -> ephemeral)
    bool bind(uint16_t port);

    // blocking recv with timeout (ms). Returns bytes read or <0 on error/timeout.
    int  recv(uint8_t* buf, size_t buf_len, Endpoint& from, int timeout_ms);

    int  send(const uint8_t* buf, size_t buf_len, const Endpoint& to);

    void close();
    bool ok() const { return sock_ != INVALID_SOCKET; }

    static std::string local_ipv4();

private:
    SOCKET sock_;
};

bool wsa_init();
void wsa_cleanup();

} // namespace fivecom
