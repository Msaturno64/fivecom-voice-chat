#pragma once
#include "network/socket.h"
#include <string>

namespace fivecom {

// Hace un STUN Binding Request a través del UdpSocket dado y devuelve el
// endpoint público (IP:puerto que ve el server STUN — es el que asignó el NAT
// para esta socket en particular).
//
// `sock` debe estar bound. Esta función envía/recibe en `sock`, así que NO
// llamarla concurrentemente con otra I/O del mismo socket.
//
// Server STUN por defecto recomendado: "stun.l.google.com" puerto 19302
// (anycast → te resuelve al PoP más cercano de Google).
bool stun_discover(UdpSocket& sock,
                   const std::string& server_host, uint16_t server_port,
                   Endpoint& public_ep, std::string& err);

// Resuelve hostname → primer IPv4. Devuelve "" si falla.
std::string resolve_ipv4(const std::string& host);

} // namespace fivecom
