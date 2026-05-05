#pragma once
#include "network/socket.h"
#include <cstdint>
#include <string>
#include <vector>

namespace fivecom {

struct RemotePeer {
    std::string ip;
    uint16_t    port = 0;
    std::string nick;
};

class Rendezvous {
public:
    std::string base_url; // sin slash final, ej. "https://x.workers.dev"

    bool create(const Endpoint& pub, const std::string& nick,
                std::string& code_out, std::string& secret_out, std::string& err);

    bool join(const std::string& code, const Endpoint& pub, const std::string& nick,
              Endpoint& master_out, std::string& master_nick_out, std::string& err);

    bool poll(const std::string& code, const std::string& secret,
              std::vector<RemotePeer>& pending_out, std::string& err);

    bool refresh(const std::string& code, const std::string& secret,
                 const Endpoint& pub, const std::string& nick, std::string& err);
};

// Resuelve la URL del worker. Lee `rendezvous_url.txt` al lado del .exe.
// Devuelve "" si no está configurado.
std::string load_rendezvous_url();

} // namespace fivecom
