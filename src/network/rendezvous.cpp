#include "network/rendezvous.h"
#include <windows.h>
#include <winhttp.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace fivecom {

// ---------- helpers de JSON (serialización + parsing mínimo) ----------

static void json_escape(std::string& out, const std::string& s) {
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
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

static size_t skip_ws(const std::string& s, size_t i) {
    while (i < s.size() && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r')) ++i;
    return i;
}

static bool find_str(const std::string& json, const std::string& key, std::string& out) {
    std::string pat = "\"" + key + "\"";
    size_t p = json.find(pat);
    if (p == std::string::npos) return false;
    p = skip_ws(json, p + pat.size());
    if (p >= json.size() || json[p] != ':') return false;
    p = skip_ws(json, p + 1);
    if (p >= json.size() || json[p] != '"') return false;
    ++p;
    out.clear();
    while (p < json.size() && json[p] != '"') {
        if (json[p] == '\\' && p + 1 < json.size()) {
            char e = json[p + 1];
            switch (e) {
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                default:   out.push_back(e);    break;
            }
            p += 2;
        } else {
            out.push_back(json[p++]);
        }
    }
    return p < json.size();
}

static bool find_num(const std::string& json, const std::string& key, long long& out) {
    std::string pat = "\"" + key + "\"";
    size_t p = json.find(pat);
    if (p == std::string::npos) return false;
    p = skip_ws(json, p + pat.size());
    if (p >= json.size() || json[p] != ':') return false;
    p = skip_ws(json, p + 1);
    char* endp = nullptr;
    out = std::strtoll(json.c_str() + p, &endp, 10);
    return endp && endp != json.c_str() + p;
}

// ---------- HTTP POST con WinHTTP ----------

static bool http_post(const std::string& url, const std::string& body,
                      std::string& resp_out, std::string& err) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
    std::wstring wurl(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, wurl.data(), wlen);
    if (!wurl.empty() && wurl.back() == 0) wurl.pop_back();

    URL_COMPONENTSW wc{};
    wc.dwStructSize = sizeof(wc);
    wchar_t whost[256] = {0}, wpath[1024] = {0};
    wc.lpszHostName     = whost; wc.dwHostNameLength    = 255;
    wc.lpszUrlPath      = wpath; wc.dwUrlPathLength     = 1023;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &wc)) {
        err = "URL inválida";
        return false;
    }

    HINTERNET hSession = WinHttpOpen(L"FiveCom/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { err = "WinHttpOpen falló"; return false; }
    WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 8000);

    HINTERNET hConn = WinHttpConnect(hSession, whost, wc.nPort, 0);
    if (!hConn) {
        WinHttpCloseHandle(hSession);
        err = "WinHttpConnect falló";
        return false;
    }

    DWORD flags = (wc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hConn, L"POST", wpath, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) {
        WinHttpCloseHandle(hConn);
        WinHttpCloseHandle(hSession);
        err = "WinHttpOpenRequest falló";
        return false;
    }

    bool ok = false;
    do {
        const wchar_t* hdr = L"Content-Type: application/json\r\n";
        if (!WinHttpSendRequest(hReq, hdr, (DWORD)-1L,
                (LPVOID)body.data(), (DWORD)body.size(),
                (DWORD)body.size(), 0)) {
            err = "WinHttpSendRequest falló (¿sin internet?)";
            break;
        }
        if (!WinHttpReceiveResponse(hReq, nullptr)) {
            err = "WinHttpReceiveResponse falló";
            break;
        }

        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(hReq,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);

        resp_out.clear();
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(hReq, &avail)) break;
            if (avail == 0) break;
            std::vector<char> chunk(avail);
            DWORD got = 0;
            if (!WinHttpReadData(hReq, chunk.data(), avail, &got)) break;
            resp_out.append(chunk.data(), got);
        }

        if (status < 200 || status >= 300) {
            char b[64];
            std::snprintf(b, sizeof(b), "HTTP %lu", (unsigned long)status);
            err = b;
            if (!resp_out.empty()) { err += " "; err += resp_out; }
            break;
        }
        ok = true;
    } while (false);

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSession);
    return ok;
}

// ---------- API pública ----------

bool Rendezvous::create(const Endpoint& pub, const std::string& nick,
                        std::string& code_out, std::string& secret_out, std::string& err) {
    if (base_url.empty()) { err = "rendezvous_url.txt vacío o ausente"; return false; }

    std::string body = "{\"public_ip\":";
    json_escape(body, pub.ip());
    body += ",\"public_port\":" + std::to_string(pub.port());
    body += ",\"nick\":";
    json_escape(body, nick);
    body += "}";

    std::string resp;
    if (!http_post(base_url + "/create", body, resp, err)) return false;

    if (!find_str(resp, "code",   code_out))   { err = "respuesta sin 'code': " + resp; return false; }
    if (!find_str(resp, "secret", secret_out)) { err = "respuesta sin 'secret'"; return false; }
    return true;
}

bool Rendezvous::join(const std::string& code, const Endpoint& pub, const std::string& nick,
                      Endpoint& master_out, std::string& master_nick_out, std::string& err) {
    if (base_url.empty()) { err = "rendezvous_url.txt vacío o ausente"; return false; }

    std::string body = "{\"code\":";
    json_escape(body, code);
    body += ",\"public_ip\":";
    json_escape(body, pub.ip());
    body += ",\"public_port\":" + std::to_string(pub.port());
    body += ",\"nick\":";
    json_escape(body, nick);
    body += "}";

    std::string resp;
    if (!http_post(base_url + "/join", body, resp, err)) return false;

    size_t mp = resp.find("\"master\"");
    if (mp == std::string::npos) { err = "respuesta sin 'master'"; return false; }
    std::string sub = resp.substr(mp);

    std::string ip;
    long long port = 0;
    if (!find_str(sub, "ip", ip))     { err = "no master.ip";   return false; }
    if (!find_num(sub, "port", port)) { err = "no master.port"; return false; }
    find_str(sub, "nick", master_nick_out);

    master_out = Endpoint::from(ip, (uint16_t)port);
    return true;
}

bool Rendezvous::poll(const std::string& code, const std::string& secret,
                      std::vector<RemotePeer>& pending_out, std::string& err) {
    pending_out.clear();
    if (base_url.empty()) { err = "rendezvous_url.txt vacío"; return false; }

    std::string body = "{\"code\":";
    json_escape(body, code);
    body += ",\"secret\":";
    json_escape(body, secret);
    body += "}";

    std::string resp;
    if (!http_post(base_url + "/poll", body, resp, err)) return false;

    size_t pp = resp.find("\"pending\"");
    if (pp == std::string::npos) return true;
    size_t lb = resp.find('[', pp);
    if (lb == std::string::npos) return true;

    size_t i = lb + 1;
    while (i < resp.size()) {
        while (i < resp.size() && (resp[i]==','||resp[i]==' '||resp[i]=='\n'||resp[i]=='\r'||resp[i]=='\t')) ++i;
        if (i >= resp.size() || resp[i] == ']') break;
        if (resp[i] != '{') { ++i; continue; }

        // Buscar la '}' que cierra este objeto, respetando strings.
        int depth = 1;
        size_t j = i + 1;
        while (j < resp.size() && depth > 0) {
            char c = resp[j];
            if (c == '"') {
                ++j;
                while (j < resp.size() && resp[j] != '"') {
                    if (resp[j] == '\\' && j + 1 < resp.size()) j += 2;
                    else ++j;
                }
                if (j < resp.size()) ++j;
            } else if (c == '{') { ++depth; ++j; }
            else if (c == '}') { --depth; ++j; }
            else ++j;
        }

        std::string obj = resp.substr(i, j - i);
        std::string ip, nick;
        long long port = 0;
        if (find_str(obj, "ip", ip) && find_num(obj, "port", port)) {
            RemotePeer rp;
            rp.ip   = ip;
            rp.port = (uint16_t)port;
            find_str(obj, "nick", rp.nick);
            pending_out.push_back(rp);
        }
        i = j;
    }
    return true;
}

bool Rendezvous::refresh(const std::string& code, const std::string& secret,
                         const Endpoint& pub, const std::string& nick, std::string& err) {
    if (base_url.empty()) { err = "rendezvous_url.txt vacío"; return false; }

    std::string body = "{\"code\":";
    json_escape(body, code);
    body += ",\"secret\":";
    json_escape(body, secret);
    body += ",\"public_ip\":";
    json_escape(body, pub.ip());
    body += ",\"public_port\":" + std::to_string(pub.port());
    body += ",\"nick\":";
    json_escape(body, nick);
    body += "}";

    std::string resp;
    return http_post(base_url + "/refresh", body, resp, err);
}

// ---------- carga del archivo de configuración ----------

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a]==' '||s[a]=='\t'||s[a]=='\r'||s[a]=='\n')) ++a;
    while (b > a && (s[b-1]==' '||s[b-1]=='\t'||s[b-1]=='\r'||s[b-1]=='\n')) --b;
    return s.substr(a, b - a);
}

// URL del worker embebida en compile-time. Se puede sobreescribir poniendo un
// `rendezvous_url.txt` al lado del .exe (útil para apuntar a otro server sin
// recompilar). Si no hay archivo, se usa esta constante.
#ifndef FIVECOM_RENDEZVOUS_URL
#define FIVECOM_RENDEZVOUS_URL ""
#endif

std::string load_rendezvous_url() {
    // 1. Override por archivo `rendezvous_url.txt` al lado del exe (opcional).
    wchar_t path[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n != 0) {
        std::wstring w(path, n);
        size_t slash = w.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            w = w.substr(0, slash + 1) + L"rendezvous_url.txt";
            std::ifstream f(w.c_str());
            if (f) {
                std::string url, line;
                while (std::getline(f, line)) {
                    line = trim(line);
                    if (!line.empty() && line[0] != '#') { url = line; break; }
                }
                while (!url.empty() && url.back() == '/') url.pop_back();
                if (!url.empty()) return url;
            }
        }
    }

    // 2. Fallback: URL embebida en compile-time.
    std::string url = FIVECOM_RENDEZVOUS_URL;
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

} // namespace fivecom
