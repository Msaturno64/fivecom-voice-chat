#include "core/config.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace fivecom {

// ---------- helpers de path ----------

static std::string config_dir() {
    const char* appdata = std::getenv("APPDATA");
    if (!appdata || !*appdata) return ".";
    std::string dir = std::string(appdata) + "\\fivecom";
    CreateDirectoryA(dir.c_str(), nullptr); // ok si ya existe
    return dir;
}

static std::string config_path() {
    return config_dir() + "\\config.json";
}

static int64_t now_epoch() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

// ---------- mini parser/serializador JSON ----------
// Suficiente para nuestra estructura plana de config. Nada de unicode complejo.

static size_t skip_ws(const std::string& s, size_t i) {
    while (i < s.size() && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r')) ++i;
    return i;
}

static bool parse_string(const std::string& s, size_t& i, std::string& out) {
    i = skip_ws(s, i);
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    out.clear();
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char e = s[i + 1];
            switch (e) {
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                default:   out.push_back(e);    break;
            }
            i += 2;
        } else {
            out.push_back(s[i++]);
        }
    }
    if (i >= s.size()) return false;
    ++i;
    return true;
}

static bool find_field_pos(const std::string& s, const std::string& key,
                           size_t scope_start, size_t scope_end, size_t& value_start) {
    std::string pat = "\"" + key + "\"";
    size_t p = s.find(pat, scope_start);
    if (p == std::string::npos || p >= scope_end) return false;
    p = skip_ws(s, p + pat.size());
    if (p >= scope_end || s[p] != ':') return false;
    value_start = skip_ws(s, p + 1);
    return value_start < scope_end;
}

static bool field_string(const std::string& s, const std::string& key,
                         size_t scope_start, size_t scope_end, std::string& out) {
    size_t v;
    if (!find_field_pos(s, key, scope_start, scope_end, v)) return false;
    return parse_string(s, v, out);
}

static bool field_int64(const std::string& s, const std::string& key,
                        size_t scope_start, size_t scope_end, int64_t& out) {
    size_t v;
    if (!find_field_pos(s, key, scope_start, scope_end, v)) return false;
    char* endp = nullptr;
    out = std::strtoll(s.c_str() + v, &endp, 10);
    return endp && endp != s.c_str() + v;
}

static void escape_into(std::string& out, const std::string& s) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", static_cast<unsigned char>(c));
                    out += b;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

// ---------- carga ----------

void config_load(Config& out) {
    out = Config{};
    std::ifstream f(config_path());
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    if (s.empty()) return;

    field_string(s, "nick", 0, s.size(), out.nick);
    field_string(s, "capture_device_id", 0, s.size(), out.capture_device_id);
    field_string(s, "render_device_id", 0, s.size(), out.render_device_id);

    int64_t n = 0;
    if (field_int64(s, "mute_hotkey_mods", 0, s.size(), n)) out.mute_hotkey_mods = (uint32_t)n;
    if (field_int64(s, "mute_hotkey_vk", 0, s.size(), n)) out.mute_hotkey_vk = (uint32_t)n;
    if (field_int64(s, "deafen_hotkey_mods", 0, s.size(), n)) out.deafen_hotkey_mods = (uint32_t)n;
    if (field_int64(s, "deafen_hotkey_vk", 0, s.size(), n)) out.deafen_hotkey_vk = (uint32_t)n;

    // last_hosted: { code, secret }
    size_t lh = s.find("\"last_hosted\"");
    if (lh != std::string::npos) {
        size_t lb = s.find('{', lh);
        if (lb != std::string::npos) {
            int depth = 1; size_t j = lb + 1;
            while (j < s.size() && depth > 0) {
                char c = s[j];
                if (c == '"') {
                    ++j;
                    while (j < s.size() && s[j] != '"') {
                        if (s[j] == '\\' && j+1 < s.size()) j += 2; else ++j;
                    }
                    if (j < s.size()) ++j;
                } else if (c == '{') { ++depth; ++j; }
                else if (c == '}') { --depth; ++j; }
                else ++j;
            }
            field_string(s, "code",   lb, j, out.last_hosted_code);
            field_string(s, "secret", lb, j, out.last_hosted_secret);
        }
    }

    // channels: [ { code, role, last_used, master_nick }, ... ]
    size_t ch = s.find("\"channels\"");
    if (ch != std::string::npos) {
        size_t lb = s.find('[', ch);
        if (lb != std::string::npos) {
            size_t i = lb + 1;
            while (i < s.size()) {
                while (i < s.size() && (s[i]==','||s[i]==' '||s[i]=='\n'||s[i]=='\r'||s[i]=='\t')) ++i;
                if (i >= s.size() || s[i] == ']') break;
                if (s[i] != '{') { ++i; continue; }
                int depth = 1; size_t j = i + 1;
                while (j < s.size() && depth > 0) {
                    char c = s[j];
                    if (c == '"') {
                        ++j;
                        while (j < s.size() && s[j] != '"') {
                            if (s[j] == '\\' && j+1 < s.size()) j += 2; else ++j;
                        }
                        if (j < s.size()) ++j;
                    } else if (c == '{') { ++depth; ++j; }
                    else if (c == '}') { --depth; ++j; }
                    else ++j;
                }
                ChannelEntry e;
                field_string(s, "code", i, j, e.code);
                field_string(s, "role", i, j, e.role);
                field_int64 (s, "last_used", i, j, e.last_used);
                field_string(s, "master_nick", i, j, e.master_nick);
                if (!e.code.empty()) out.channels.push_back(std::move(e));
                i = j;
            }
        }
    }
}

// ---------- guardado ----------

void config_save(const Config& cfg) {
    std::string out;
    out.reserve(512);
    out += "{\n  \"nick\": ";
    escape_into(out, cfg.nick);
    out += ",\n  \"capture_device_id\": ";
    escape_into(out, cfg.capture_device_id);
    out += ",\n  \"render_device_id\": ";
    escape_into(out, cfg.render_device_id);
    out += ",\n  \"mute_hotkey_mods\": ";
    out += std::to_string(cfg.mute_hotkey_mods);
    out += ",\n  \"mute_hotkey_vk\": ";
    out += std::to_string(cfg.mute_hotkey_vk);
    out += ",\n  \"deafen_hotkey_mods\": ";
    out += std::to_string(cfg.deafen_hotkey_mods);
    out += ",\n  \"deafen_hotkey_vk\": ";
    out += std::to_string(cfg.deafen_hotkey_vk);
    out += ",\n  \"last_hosted\": ";
    if (cfg.last_hosted_code.empty()) {
        out += "null";
    } else {
        out += "{ \"code\": ";
        escape_into(out, cfg.last_hosted_code);
        out += ", \"secret\": ";
        escape_into(out, cfg.last_hosted_secret);
        out += " }";
    }
    out += ",\n  \"channels\": [";
    for (size_t k = 0; k < cfg.channels.size(); ++k) {
        const auto& e = cfg.channels[k];
        out += (k == 0) ? "\n    " : ",\n    ";
        out += "{ \"code\": ";
        escape_into(out, e.code);
        out += ", \"role\": ";
        escape_into(out, e.role);
        out += ", \"last_used\": ";
        out += std::to_string(e.last_used);
        out += ", \"master_nick\": ";
        escape_into(out, e.master_nick);
        out += " }";
    }
    if (!cfg.channels.empty()) out += "\n  ";
    out += "]\n}\n";

    std::ofstream f(config_path(), std::ios::binary | std::ios::trunc);
    if (!f) return;
    f.write(out.data(), (std::streamsize)out.size());
}

// ---------- helpers de historial ----------

static void touch_channel(Config& cfg, const std::string& code, const std::string& role,
                          const std::string& master_nick) {
    // Quitar duplicados del mismo code.
    cfg.channels.erase(std::remove_if(cfg.channels.begin(), cfg.channels.end(),
        [&](const ChannelEntry& e){ return e.code == code; }), cfg.channels.end());

    ChannelEntry e;
    e.code = code;
    e.role = role;
    e.last_used = now_epoch();
    e.master_nick = master_nick;
    cfg.channels.insert(cfg.channels.begin(), std::move(e));

    // Limitar a 20.
    if (cfg.channels.size() > 20) cfg.channels.resize(20);
}

void config_record_hosted(Config& cfg, const std::string& code) {
    touch_channel(cfg, code, "host", "");
}

void config_record_joined(Config& cfg, const std::string& code, const std::string& master_nick) {
    touch_channel(cfg, code, "client", master_nick);
}

} // namespace fivecom
