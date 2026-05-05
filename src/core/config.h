#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace fivecom {

struct ChannelEntry {
    std::string code;
    std::string role;        // "host" o "client"
    int64_t     last_used = 0; // epoch seconds
    std::string master_nick; // sólo para entries de "client"
};

struct Config {
    std::string nick = "user";
    std::string capture_device_id;
    std::string render_device_id;
    uint32_t mute_hotkey_mods = 0;
    uint32_t mute_hotkey_vk = 0;
    uint32_t deafen_hotkey_mods = 0;
    uint32_t deafen_hotkey_vk = 0;
    // Último canal hosteado por este usuario (para reusar el mismo código).
    std::string last_hosted_code;
    std::string last_hosted_secret;
    // Historial de todos los canales (hosteados + unidos), más nuevo primero.
    std::vector<ChannelEntry> channels;
};

// Lee config desde %APPDATA%\fivecom\config.json. Si no existe, devuelve defaults.
void config_load(Config& out);

// Persiste config (crea la carpeta si hace falta).
void config_save(const Config& cfg);

// Helpers que mantienen `channels` en orden (más reciente primero, sin duplicados).
void config_record_hosted(Config& cfg, const std::string& code);
void config_record_joined(Config& cfg, const std::string& code, const std::string& master_nick);

} // namespace fivecom
