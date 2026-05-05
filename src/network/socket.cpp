#include "network/socket.h"
#include <iphlpapi.h>
#include <cstring>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace fivecom {

bool wsa_init() {
    WSADATA d;
    return WSAStartup(MAKEWORD(2, 2), &d) == 0;
}
void wsa_cleanup() { WSACleanup(); }

Endpoint Endpoint::from(const std::string& ip, uint16_t port) {
    Endpoint e;
    e.addr.sin_family = AF_INET;
    e.addr.sin_port   = htons(port);
    inet_pton(AF_INET, ip.c_str(), &e.addr.sin_addr);
    return e;
}

std::string Endpoint::ip() const {
    char buf[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
    return buf;
}

UdpSocket::UdpSocket() : sock_(INVALID_SOCKET) {}
UdpSocket::~UdpSocket() { close(); }

bool UdpSocket::bind(uint16_t port) {
    sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == INVALID_SOCKET) return false;

    int rcv_buf = 256 * 1024;
    setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, (const char*)&rcv_buf, sizeof(rcv_buf));
    int snd_buf = 256 * 1024;
    setsockopt(sock_, SOL_SOCKET, SO_SNDBUF, (const char*)&snd_buf, sizeof(snd_buf));

    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port        = htons(port);
    if (::bind(sock_, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
        return false;
    }
    return true;
}

int UdpSocket::recv(uint8_t* buf, size_t buf_len, Endpoint& from, int timeout_ms) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(sock_, &rfds);
    timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int sel = select(0, &rfds, nullptr, nullptr, &tv);
    if (sel <= 0) return -1;

    int from_len = sizeof(from.addr);
    int n = ::recvfrom(sock_, (char*)buf, (int)buf_len, 0,
                       (sockaddr*)&from.addr, &from_len);
    return n;
}

int UdpSocket::send(const uint8_t* buf, size_t buf_len, const Endpoint& to) {
    return ::sendto(sock_, (const char*)buf, (int)buf_len, 0,
                    (const sockaddr*)&to.addr, sizeof(to.addr));
}

void UdpSocket::close() {
    if (sock_ != INVALID_SOCKET) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
}

std::string UdpSocket::local_ipv4() {
    // Pick the first non-loopback IPv4 from GetAdaptersAddresses.
    ULONG buf_size = 16 * 1024;
    std::vector<uint8_t> buf(buf_size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &buf_size) != NO_ERROR) {
        return "127.0.0.1";
    }
    for (auto* a = adapters; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
            auto* sa = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
            if (sa->sin_family != AF_INET) continue;
            char ip[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
            std::string s = ip;
            if (s != "127.0.0.1" && s.rfind("169.254.", 0) != 0) {
                return s;
            }
        }
    }
    return "127.0.0.1";
}

} // namespace fivecom
