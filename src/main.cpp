#include <windows.h>
#include <commctrl.h>
#include <mmsystem.h>

#include "core/app.h"
#include "core/config.h"
#include "core/log.h"
#include "core/master.h"
#include "core/client.h"
#include "audio/wasapi.h"
#include "network/socket.h"
#include "network/packet.h"
#include "resource.h"

#include <string>
#include <memory>
#include <vector>
#include <cctype>
#include <algorithm>
#include <cmath>
#include <cstring>

#pragma comment(lib, "comctl32.lib")

using namespace fivecom;

namespace {

// ---- IDs de controles ----
constexpr int ID_NICK     = 1001;
constexpr int ID_IP       = 1002;
constexpr int ID_BTN_HOST = 1003;
constexpr int ID_BTN_JOIN = 1004;
constexpr int ID_BTN_LEAVE= 1005;
constexpr int ID_BTN_MUTE = 1006;
constexpr int ID_PEERS    = 1007;
constexpr int ID_STATUS   = 1008;
constexpr int ID_BTN_HIST = 1009;
constexpr int ID_BTN_NEW  = 1010;
constexpr int ID_CAPTURE  = 1011;
constexpr int ID_RENDER   = 1012;
constexpr int ID_BTN_DEAFEN = 1013;
constexpr int ID_BTN_SET_MUTE = 1014;
constexpr int ID_BTN_SET_DEAFEN = 1015;
constexpr int ID_BTN_CLEAR_MUTE = 1016;
constexpr int ID_BTN_CLEAR_DEAFEN = 1017;
constexpr int ID_BTN_SETTINGS = 1018;
constexpr int ID_HOTKEY_MUTE = 3001;
constexpr int ID_HOTKEY_DEAFEN = 3002;
constexpr int ID_TIMER    = 1;
constexpr int ID_HIST_BASE = 2000; // ids dinámicos del popup

HWND g_hwnd       = nullptr;
HWND g_edit_nick  = nullptr;
HWND g_edit_ip    = nullptr;
HWND g_btn_host   = nullptr;
HWND g_btn_join   = nullptr;
HWND g_btn_leave  = nullptr;
HWND g_btn_mute   = nullptr;
HWND g_btn_hist   = nullptr;
HWND g_btn_new    = nullptr;
HWND g_capture    = nullptr;
HWND g_render     = nullptr;
HWND g_btn_deafen = nullptr;
HWND g_btn_set_mute = nullptr;
HWND g_btn_set_deafen = nullptr;
HWND g_btn_clear_mute = nullptr;
HWND g_btn_clear_deafen = nullptr;
HWND g_btn_settings = nullptr;
HWND g_label_nick = nullptr;
HWND g_label_code = nullptr;
HWND g_peers      = nullptr;
HWND g_status     = nullptr;

HFONT g_font_main = nullptr;
HFONT g_font_title = nullptr;
HFONT g_font_small = nullptr;
HBRUSH g_brush_bg = nullptr;
HBRUSH g_brush_panel = nullptr;
HBRUSH g_brush_field = nullptr;
HICON g_icon_leave = nullptr;
HICON g_icon_mic = nullptr;
HICON g_icon_mic_muted = nullptr;
HICON g_icon_headphones = nullptr;
HICON g_icon_headphones_muted = nullptr;

constexpr COLORREF C_BG       = RGB(10, 14, 23);
constexpr COLORREF C_PANEL    = RGB(18, 24, 37);
constexpr COLORREF C_PANEL_2  = RGB(23, 31, 47);
constexpr COLORREF C_FIELD    = RGB(11, 17, 28);
constexpr COLORREF C_TEXT     = RGB(232, 238, 247);
constexpr COLORREF C_MUTED    = RGB(139, 152, 174);
constexpr COLORREF C_BORDER   = RGB(45, 56, 78);
constexpr COLORREF C_ACCENT   = RGB(73, 211, 161);
constexpr COLORREF C_ACCENT_2 = RGB(51, 133, 255);
constexpr COLORREF C_DANGER   = RGB(246, 95, 95);

// Snapshot del historial mientras está abierto el popup, indexado por ID_HIST_BASE+i.
std::vector<ChannelEntry> g_hist_snapshot;
std::vector<AudioDeviceInfo> g_capture_devices;
std::vector<AudioDeviceInfo> g_render_devices;
Config g_cfg;
int g_capture_hotkey = 0; // 0 ninguno, ID_HOTKEY_MUTE/DEAFEN esperando tecla
bool g_muted_before_deafen = false;
bool g_settings_open = false;

enum class UiSound { Mute, Unmute, Deafen, Undeafen };

std::vector<char> make_tone_wav(std::initializer_list<int> freqs, int tone_ms = 95) {
    constexpr int sample_rate = 44100;
    constexpr int channels = 1;
    constexpr int bits = 16;
    constexpr int bytes_per_sample = bits / 8;
    int samples_per_tone = sample_rate * tone_ms / 1000;
    int total_samples = samples_per_tone * (int)freqs.size();
    int data_bytes = total_samples * channels * bytes_per_sample;
    std::vector<char> wav(44 + data_bytes, 0);

    auto put16 = [&](int off, uint16_t v) {
        wav[off] = (char)(v & 0xff);
        wav[off + 1] = (char)((v >> 8) & 0xff);
    };
    auto put32 = [&](int off, uint32_t v) {
        wav[off] = (char)(v & 0xff);
        wav[off + 1] = (char)((v >> 8) & 0xff);
        wav[off + 2] = (char)((v >> 16) & 0xff);
        wav[off + 3] = (char)((v >> 24) & 0xff);
    };

    std::memcpy(wav.data() + 0, "RIFF", 4);
    put32(4, 36 + data_bytes);
    std::memcpy(wav.data() + 8, "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, channels);
    put32(24, sample_rate);
    put32(28, sample_rate * channels * bytes_per_sample);
    put16(32, channels * bytes_per_sample);
    put16(34, bits);
    std::memcpy(wav.data() + 36, "data", 4);
    put32(40, data_bytes);

    int cursor = 44;
    for (int freq : freqs) {
        for (int i = 0; i < samples_per_tone; ++i) {
            double t = (double)i / sample_rate;
            double env = 1.0;
            int fade = std::max(1, samples_per_tone / 8);
            if (i < fade) env = (double)i / fade;
            else if (i > samples_per_tone - fade) env = (double)(samples_per_tone - i) / fade;
            int16_t sample = (int16_t)(std::sin(2.0 * 3.14159265358979323846 * freq * t) * env * 9000.0);
            wav[cursor++] = (char)(sample & 0xff);
            wav[cursor++] = (char)((sample >> 8) & 0xff);
        }
    }
    return wav;
}

void play_ui_sound(UiSound sound) {
    static const std::vector<char> mute = make_tone_wav({520, 360});
    static const std::vector<char> unmute = make_tone_wav({360, 560});
    static const std::vector<char> deafen = make_tone_wav({620, 440, 260}, 80);
    static const std::vector<char> undeafen = make_tone_wav({260, 440, 700}, 80);
    const std::vector<char>* wav = &mute;
    if (sound == UiSound::Unmute) wav = &unmute;
    else if (sound == UiSound::Deafen) wav = &deafen;
    else if (sound == UiSound::Undeafen) wav = &undeafen;
    PlaySoundA(wav->data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

void load_control_icons() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    g_icon_leave = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_BTN_LEAVE), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    g_icon_mic = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_BTN_MIC), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    g_icon_mic_muted = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_BTN_MIC_MUTED), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    g_icon_headphones = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_BTN_HEADPHONES), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    g_icon_headphones_muted = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_BTN_HEADPHONES_MUTED), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
}

std::unique_ptr<MasterNode> g_master;
std::unique_ptr<ClientNode> g_client;

std::wstring s2w(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    if (!w.empty() && w.back() == 0) w.pop_back();
    return w;
}

std::string w2s(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == 0) s.pop_back();
    return s;
}

std::string get_edit_text(HWND h) {
    int len = GetWindowTextLengthW(h);
    std::wstring w(len + 1, 0);
    GetWindowTextW(h, w.data(), len + 1);
    w.resize(wcslen(w.c_str()));
    return w2s(w);
}

std::wstring hotkey_text(uint32_t mods, uint32_t vk) {
    if (!vk) return L"Sin asignar";
    std::wstring s;
    if (mods & MOD_CONTROL) s += L"Ctrl+";
    if (mods & MOD_SHIFT) s += L"Shift+";
    if (mods & MOD_ALT) s += L"Alt+";
    wchar_t name[64]{};
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16;
    if (GetKeyNameTextW((LONG)scan, name, 64) > 0) s += name;
    else s += L"VK" + std::to_wstring(vk);
    return s;
}

void save_ui_config() {
    g_cfg.nick = get_edit_text(g_edit_nick);
    int cap = (int)SendMessageW(g_capture, CB_GETCURSEL, 0, 0);
    int ren = (int)SendMessageW(g_render, CB_GETCURSEL, 0, 0);
    g_cfg.capture_device_id = cap > 0 && (size_t)(cap - 1) < g_capture_devices.size()
        ? g_capture_devices[(size_t)cap - 1].id : "";
    g_cfg.render_device_id = ren > 0 && (size_t)(ren - 1) < g_render_devices.size()
        ? g_render_devices[(size_t)ren - 1].id : "";
    config_save(g_cfg);
}

void refresh_hotkey_buttons() {
    if (!g_btn_set_mute) return;
    SetWindowTextW(g_btn_set_mute,
        g_capture_hotkey == ID_HOTKEY_MUTE ? L"Presioná teclas..." : hotkey_text(g_cfg.mute_hotkey_mods, g_cfg.mute_hotkey_vk).c_str());
    SetWindowTextW(g_btn_set_deafen,
        g_capture_hotkey == ID_HOTKEY_DEAFEN ? L"Presioná teclas..." : hotkey_text(g_cfg.deafen_hotkey_mods, g_cfg.deafen_hotkey_vk).c_str());
}

void update_settings_panel() {
    HWND settings_controls[] = {
        g_capture, g_render, g_btn_set_mute, g_btn_set_deafen,
        g_btn_clear_mute, g_btn_clear_deafen
    };
    for (HWND h : settings_controls) {
        if (h) ShowWindow(h, g_settings_open ? SW_SHOW : SW_HIDE);
        if (h && g_settings_open) SetWindowPos(h, HWND_TOP, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    HWND main_controls[] = {
        g_label_nick, g_label_code, g_edit_nick, g_edit_ip, g_btn_hist,
        g_btn_host, g_btn_new, g_btn_join, g_btn_leave, g_btn_mute, g_btn_deafen, g_peers
    };
    for (HWND h : main_controls) {
        if (h) ShowWindow(h, g_settings_open ? SW_HIDE : SW_SHOW);
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

void register_hotkeys() {
    if (!g_hwnd) return;
    UnregisterHotKey(g_hwnd, ID_HOTKEY_MUTE);
    UnregisterHotKey(g_hwnd, ID_HOTKEY_DEAFEN);
    if (g_cfg.mute_hotkey_vk) {
        RegisterHotKey(g_hwnd, ID_HOTKEY_MUTE, g_cfg.mute_hotkey_mods | MOD_NOREPEAT, g_cfg.mute_hotkey_vk);
    }
    if (g_cfg.deafen_hotkey_vk) {
        RegisterHotKey(g_hwnd, ID_HOTKEY_DEAFEN, g_cfg.deafen_hotkey_mods | MOD_NOREPEAT, g_cfg.deafen_hotkey_vk);
    }
}

void save_hotkey_config() {
    Config cur;
    config_load(cur);
    cur.mute_hotkey_mods = g_cfg.mute_hotkey_mods;
    cur.mute_hotkey_vk = g_cfg.mute_hotkey_vk;
    cur.deafen_hotkey_mods = g_cfg.deafen_hotkey_mods;
    cur.deafen_hotkey_vk = g_cfg.deafen_hotkey_vk;
    config_save(cur);
    g_cfg = cur;
}

void populate_device_combo(HWND combo, std::vector<AudioDeviceInfo>& devices,
                           bool capture, const std::string& selected_id) {
    devices = list_audio_devices(capture);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Predeterminado de Windows");
    int selected = 0;
    for (size_t i = 0; i < devices.size(); ++i) {
        std::wstring name = s2w(devices[i].name);
        if (devices[i].is_default) name += L"  (actual)";
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)name.c_str());
        if (!selected_id.empty() && devices[i].id == selected_id) selected = (int)i + 1;
    }
    SendMessageW(combo, CB_SETCURSEL, selected, 0);
}

void update_buttons() {
    Mode m = AppState::instance().mode.load();
    EnableWindow(g_btn_host,  m == Mode::Idle);
    EnableWindow(g_btn_new,   m == Mode::Idle);
    EnableWindow(g_btn_join,  m == Mode::Idle);
    EnableWindow(g_btn_leave, m != Mode::Idle);
    EnableWindow(g_btn_mute,  m != Mode::Idle);
    EnableWindow(g_btn_deafen, m != Mode::Idle);
    EnableWindow(g_edit_nick, m == Mode::Idle);
    EnableWindow(g_edit_ip,   m == Mode::Idle);
    EnableWindow(g_capture,   m == Mode::Idle);
    EnableWindow(g_render,    m == Mode::Idle);
    EnableWindow(g_btn_hist,  m == Mode::Idle);

    SetWindowTextW(g_btn_mute,
        AppState::instance().muted.load() ? L"" : L"");
    SetWindowTextW(g_btn_deafen, L"");
    SetWindowTextW(g_btn_settings, g_settings_open ? L"x" : L"⚙");
}

void set_status(const std::wstring& text) {
    SetWindowTextW(g_status, text.c_str());
}

void fill_rect(HDC dc, const RECT& r, COLORREF color) {
    HBRUSH b = CreateSolidBrush(color);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void draw_round_rect(HDC dc, RECT r, COLORREF fill, COLORREF border, int radius = 14) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ old_b = SelectObject(dc, b);
    HGDIOBJ old_p = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, old_p);
    SelectObject(dc, old_b);
    DeleteObject(p);
    DeleteObject(b);
}

void draw_text(HDC dc, const wchar_t* text, RECT r, HFONT font, COLORREF color,
               UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
    HGDIOBJ old_font = SelectObject(dc, font ? font : g_font_main);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &r, format);
    SelectObject(dc, old_font);
}

void draw_shell(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    fill_rect(dc, rc, C_BG);

    RECT hero{18, 16, rc.right - 18, 82};
    draw_round_rect(dc, hero, RGB(13, 20, 34), RGB(36, 50, 78), 18);
    RECT glow{hero.left + 1, hero.bottom - 3, hero.right - 1, hero.bottom - 1};
    fill_rect(dc, glow, C_ACCENT_2);

    RECT title{34, 25, rc.right - 86, 49};
    draw_text(dc, L"FiveCom", title, g_font_title, C_TEXT);
    RECT subtitle{34, 51, rc.right - 86, 74};
    draw_text(dc, L"P2P voice chat · Opus · WASAPI · UDP", subtitle, g_font_small, C_MUTED);

    RECT connect{18, 96, rc.right - 18, 245};
    draw_round_rect(dc, connect, C_PANEL, C_BORDER, 16);
    RECT connect_title{34, 109, rc.right - 34, 132};
    draw_text(dc, L"Conexión", connect_title, g_font_main, C_TEXT);

    RECT people{18, 260, rc.right - 18, rc.bottom - 42};
    draw_round_rect(dc, people, C_PANEL, C_BORDER, 16);
    RECT people_title{34, 273, rc.right - 34, 296};
    draw_text(dc, L"Voces en el canal", people_title, g_font_main, C_TEXT);

    RECT nick_field{30, 159, 248, 197};
    draw_round_rect(dc, nick_field, C_FIELD, C_BORDER, 12);
    RECT code_field{262, 159, 412, 197};
    draw_round_rect(dc, code_field, C_FIELD, C_BORDER, 12);
    RECT list_field{30, 298, 546, rc.bottom - 58};
    draw_round_rect(dc, list_field, C_PANEL_2, C_BORDER, 14);

    if (g_settings_open) {
        RECT shade{0, 0, rc.right, rc.bottom};
        HBRUSH shade_brush = CreateSolidBrush(RGB(7, 10, 18));
        FillRect(dc, &shade, shade_brush);
        DeleteObject(shade_brush);

        RECT panel{54, 104, rc.right - 54, 438};
        draw_round_rect(dc, panel, C_PANEL, RGB(66, 84, 120), 18);
        RECT panel_title{76, 124, rc.right - 76, 150};
        draw_text(dc, L"Configuración", panel_title, g_font_title, C_TEXT);
        RECT panel_sub{76, 153, rc.right - 76, 176};
        draw_text(dc, L"Audio, salida y atajos globales", panel_sub, g_font_small, C_MUTED);
        RECT cap_box{72, 198, rc.right - 72, 258};
        draw_round_rect(dc, cap_box, C_PANEL_2, C_BORDER, 14);
        RECT hot_box{72, 278, rc.right - 72, 414};
        draw_round_rect(dc, hot_box, C_PANEL_2, C_BORDER, 14);
        RECT cap_label{88, 206, 210, 229};
        draw_text(dc, L"Micrófono", cap_label, g_font_small, C_MUTED);
        RECT ren_label{320, 206, 450, 229};
        draw_text(dc, L"Altavoces", ren_label, g_font_small, C_MUTED);
        RECT hot_label{88, 290, 300, 313};
        draw_text(dc, L"Atajos globales", hot_label, g_font_small, C_MUTED);
        RECT mute_label{120, 326, 220, 349};
        draw_text(dc, L"Mute", mute_label, g_font_small, C_MUTED);
        RECT deaf_label{120, 370, 240, 393};
        draw_text(dc, L"Sordera", deaf_label, g_font_small, C_MUTED);
    }

    EndPaint(hwnd, &ps);
}

void draw_button(const DRAWITEMSTRUCT* dis) {
    HDC dc = dis->hDC;
    RECT r = dis->rcItem;
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;

    wchar_t text[128]{};
    GetWindowTextW(dis->hwndItem, text, 128);

    COLORREF fill = C_PANEL_2;
    COLORREF border = C_BORDER;
    COLORREF text_color = disabled ? RGB(83, 94, 115) : C_TEXT;

    if (!disabled) {
        if (dis->CtlID == ID_BTN_HOST || dis->CtlID == ID_BTN_NEW) {
            fill = pressed ? RGB(45, 151, 118) : RGB(38, 126, 101);
            border = RGB(74, 214, 165);
        } else if (dis->CtlID == ID_BTN_JOIN) {
            fill = pressed ? RGB(42, 103, 194) : RGB(36, 86, 166);
            border = RGB(78, 143, 255);
        } else if (dis->CtlID == ID_BTN_LEAVE) {
            fill = pressed ? RGB(126, 47, 51) : RGB(96, 39, 45);
            border = C_DANGER;
        }
    }

    draw_round_rect(dc, r, fill, border, 12);
    if (pressed && !disabled) OffsetRect(&r, 1, 1);

    if (dis->CtlID == ID_BTN_LEAVE || dis->CtlID == ID_BTN_MUTE || dis->CtlID == ID_BTN_DEAFEN) {
        HICON icon = g_icon_leave;
        if (dis->CtlID == ID_BTN_MUTE) {
            icon = AppState::instance().muted.load() ? g_icon_mic_muted : g_icon_mic;
        } else if (dis->CtlID == ID_BTN_DEAFEN) {
            icon = AppState::instance().deafened.load() ? g_icon_headphones_muted : g_icon_headphones;
        }
        int size = 24;
        int x = r.left + ((r.right - r.left) - size) / 2;
        int y = r.top + ((r.bottom - r.top) - size) / 2;
        if (icon) DrawIconEx(dc, x, y, icon, size, size, 0, nullptr, DI_NORMAL);
        return;
    }

    draw_text(dc, text, r, g_font_main, text_color, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void draw_peer_item(const DRAWITEMSTRUCT* dis) {
    if (dis->itemID == (UINT)-1) return;
    HDC dc = dis->hDC;
    RECT r = dis->rcItem;
    bool selected = (dis->itemState & ODS_SELECTED) != 0;
    fill_rect(dc, r, selected ? RGB(31, 47, 75) : C_PANEL);

    wchar_t raw[256]{};
    SendMessageW(dis->hwndItem, LB_GETTEXT, dis->itemID, (LPARAM)raw);
    std::wstring line = raw;
    bool speaking = line.rfind(L"[*]", 0) == 0;
    if (line.size() >= 4 && line[0] == L'[') line = line.substr(4);

    RECT dot{r.left + 14, r.top + 10, r.left + 26, r.top + 22};
    HBRUSH dot_brush = CreateSolidBrush(speaking ? C_ACCENT : RGB(67, 78, 99));
    HPEN dot_pen = CreatePen(PS_SOLID, 1, speaking ? RGB(111, 239, 195) : RGB(83, 94, 115));
    HGDIOBJ old_b = SelectObject(dc, dot_brush);
    HGDIOBJ old_p = SelectObject(dc, dot_pen);
    Ellipse(dc, dot.left, dot.top, dot.right, dot.bottom);
    SelectObject(dc, old_p);
    SelectObject(dc, old_b);
    DeleteObject(dot_pen);
    DeleteObject(dot_brush);

    RECT text_r{r.left + 38, r.top, r.right - 14, r.bottom};
    draw_text(dc, line.c_str(), text_r, g_font_main, selected ? RGB(246, 250, 255) : C_TEXT);
}

void refresh_status() {
    Mode m = AppState::instance().mode.load();
    std::wstring s;
    switch (m) {
        case Mode::Idle:
            s = L"Desconectado";
            break;
        case Mode::Master:
            s = L"Master | código: " + s2w(AppState::instance().channel_code)
              + L" | público: " + s2w(AppState::instance().public_ip) + L":"
              + std::to_wstring(AppState::instance().public_port);
            break;
        case Mode::Client:
            s = L"Cliente | canal " + s2w(AppState::instance().channel_code)
              + L" | master " + s2w(AppState::instance().master_ip);
            break;
    }
    if (m != Mode::Idle && AppState::instance().muted.load()) s += L" | MUTEADO";
    if (m != Mode::Idle && AppState::instance().deafened.load()) s += L" | ENSORDECIDO";
    set_status(s);
}

void refresh_peers() {
    auto peers = AppState::instance().get_peers();
    int sel = (int)SendMessageW(g_peers, LB_GETCURSEL, 0, 0);
    SendMessageW(g_peers, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_peers, LB_RESETCONTENT, 0, 0);

    uint32_t my_id = AppState::instance().self_id.load();
    bool self_speaking = AppState::instance().self_speaking.load();

    if (peers.empty() && AppState::instance().mode.load() != Mode::Idle) {
        // mostrar al menos a uno mismo
        std::wstring line = (self_speaking ? L"[*] " : L"[ ] ")
                          + s2w(AppState::instance().nick) + L" (vos)";
        SendMessageW(g_peers, LB_ADDSTRING, 0, (LPARAM)line.c_str());
    }
    for (auto& p : peers) {
        bool is_self = (p.id == my_id);
        bool sp = is_self ? self_speaking : p.speaking;
        std::wstring line = (sp ? L"[*] " : L"[ ] ") + s2w(p.nick);
        if (is_self) line += L" (vos)";
        SendMessageW(g_peers, LB_ADDSTRING, 0, (LPARAM)line.c_str());
    }
    if (sel >= 0) SendMessageW(g_peers, LB_SETCURSEL, sel, 0);
    SendMessageW(g_peers, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_peers, nullptr, FALSE);
}

void on_host() {
    log_msg("on_host: click");
    try {
        std::string nick = get_edit_text(g_edit_nick);
        if (nick.empty()) nick = "host";
        AppState::instance().nick = nick;
        save_ui_config();
        AppState::instance().local_ip = UdpSocket::local_ipv4();
        log_msg("on_host: nick='%s' local_ip='%s'", nick.c_str(),
                AppState::instance().local_ip.c_str());

        g_master = std::make_unique<MasterNode>();
        std::string err;
        log_msg("on_host: calling MasterNode::start");
        if (!g_master->start(DEFAULT_PORT, nick, err)) {
            log_msg("on_host: master start FAILED: %s", err.c_str());
            MessageBoxW(g_hwnd, s2w(err).c_str(), L"Error al crear canal", MB_ICONERROR);
            g_master.reset();
            return;
        }
        log_msg("on_host: master started OK code=%s",
                AppState::instance().channel_code.c_str());
        AppState::instance().mode.store(Mode::Master);
        // Mostrar el código en el campo "Código" para que el usuario lo copie.
        SetWindowTextW(g_edit_ip, s2w(AppState::instance().channel_code).c_str());
        update_buttons();
        refresh_status();
    } catch (const std::exception& e) {
        log_msg("on_host EXCEPTION: %s", e.what());
        MessageBoxA(g_hwnd, e.what(), "Excepción en on_host", MB_ICONERROR);
    } catch (...) {
        log_msg("on_host UNKNOWN EXCEPTION");
        MessageBoxW(g_hwnd, L"Excepción desconocida en on_host", L"Error", MB_ICONERROR);
    }
}

void on_join() {
    log_msg("on_join: click");
    try {
        std::string nick = get_edit_text(g_edit_nick);
        std::string code = get_edit_text(g_edit_ip);
        if (nick.empty()) nick = "user";
        if (code.empty()) {
            MessageBoxW(g_hwnd, L"Ingresá el código del canal",
                        L"FiveCom", MB_ICONWARNING);
            return;
        }
        // Normalizar a mayúsculas (los códigos del rendezvous son uppercase).
        for (auto& c : code) c = (char)toupper((unsigned char)c);

        AppState::instance().nick = nick;
        AppState::instance().channel_code = code;
        save_ui_config();
        log_msg("on_join: nick='%s' code='%s'", nick.c_str(), code.c_str());

        g_client = std::make_unique<ClientNode>();
        std::string err;
        log_msg("on_join: calling ClientNode::start");
        if (!g_client->start(code, nick, err)) {
            log_msg("on_join: client start FAILED: %s", err.c_str());
            MessageBoxW(g_hwnd, s2w(err).c_str(), L"Error al unirse", MB_ICONERROR);
            g_client.reset();
            return;
        }
        log_msg("on_join: client started OK");
        AppState::instance().mode.store(Mode::Client);
        update_buttons();
        refresh_status();
    } catch (const std::exception& e) {
        log_msg("on_join EXCEPTION: %s", e.what());
        MessageBoxA(g_hwnd, e.what(), "Excepción en on_join", MB_ICONERROR);
    } catch (...) {
        log_msg("on_join UNKNOWN EXCEPTION");
        MessageBoxW(g_hwnd, L"Excepción desconocida en on_join", L"Error", MB_ICONERROR);
    }
}

void on_leave() {
    if (g_master) { g_master->stop(); g_master.reset(); }
    if (g_client) { g_client->stop(); g_client.reset(); }
    AppState::instance().mode.store(Mode::Idle);
    AppState::instance().self_id.store(0);
    AppState::instance().muted.store(false);
    AppState::instance().deafened.store(false);
    g_muted_before_deafen = false;
    AppState::instance().self_speaking.store(false);
    AppState::instance().channel_code.clear();
    AppState::instance().channel_secret.clear();
    AppState::instance().set_peers({});
    SetWindowTextW(g_edit_ip, L"");
    update_buttons();
    refresh_status();
    refresh_peers();
}

void on_mute() {
    if (AppState::instance().deafened.load()) {
        AppState::instance().muted.store(true);
    } else {
        bool muted = !AppState::instance().muted.load();
        AppState::instance().muted.store(muted);
        play_ui_sound(muted ? UiSound::Mute : UiSound::Unmute);
    }
    update_buttons();
    refresh_status();
}

void on_deafen() {
    bool d = !AppState::instance().deafened.load();
    AppState::instance().deafened.store(d);
    if (d) {
        g_muted_before_deafen = AppState::instance().muted.load();
        AppState::instance().muted.store(true);
    } else {
        AppState::instance().muted.store(g_muted_before_deafen);
    }
    play_ui_sound(d ? UiSound::Deafen : UiSound::Undeafen);
    update_buttons();
    refresh_status();
}

void on_new_code() {
    // Forzar generación de código nuevo: borrar el guardado y hostear.
    Config cfg;
    config_load(cfg);
    cfg.last_hosted_code.clear();
    cfg.last_hosted_secret.clear();
    config_save(cfg);
    on_host();
}

void on_failover_to_master() {
    if (!g_client) return;

    // Snapshot del estado del cliente antes de derribarlo.
    std::string code   = g_client->channel_code();
    std::string secret = g_client->channel_secret();
    std::string nick   = AppState::instance().nick;
    auto cps = g_client->snapshot_peers();

    uint32_t my_id = AppState::instance().self_id.load();
    std::vector<InheritedPeer> inherited;
    inherited.reserve(cps.size());
    for (auto& cp : cps) {
        if (cp.id == my_id) continue; // soy yo
        if (cp.id == 1) continue;     // master muerto
        InheritedPeer ip;
        ip.id   = cp.id;
        ip.nick = cp.nick;
        ip.ep   = cp.ep;
        inherited.push_back(ip);
    }
    log_msg("failover: cliente -> master, peers heredados=%zu", inherited.size());

    g_client->stop();
    g_client.reset();

    g_master = std::make_unique<MasterNode>();
    std::string err;
    if (!g_master->start_failover(code, secret, nick, inherited, err)) {
        log_msg("failover: start_failover falló: %s", err.c_str());
        MessageBoxW(g_hwnd, s2w(err).c_str(), L"Failover falló", MB_ICONERROR);
        g_master.reset();
        AppState::instance().mode.store(Mode::Idle);
        AppState::instance().self_id.store(0);
        AppState::instance().channel_code.clear();
        AppState::instance().channel_secret.clear();
        update_buttons();
        refresh_status();
        return;
    }

    AppState::instance().mode.store(Mode::Master);
    SetWindowTextW(g_edit_ip, s2w(AppState::instance().channel_code).c_str());
    update_buttons();
    refresh_status();
}

void on_history() {
    // Construir popup menu con el historial.
    Config cfg;
    config_load(cfg);
    g_hist_snapshot = cfg.channels;
    if (g_hist_snapshot.empty()) {
        MessageBoxW(g_hwnd, L"No hay canales en el historial todavía.",
                    L"Historial", MB_ICONINFORMATION);
        return;
    }

    HMENU menu = CreatePopupMenu();
    for (size_t i = 0; i < g_hist_snapshot.size() && i < 20; ++i) {
        const auto& e = g_hist_snapshot[i];
        std::wstring item = s2w(e.code);
        if (e.role == "host") item += L"  (creado por vos)";
        else if (!e.master_nick.empty()) item += L"  (master: " + s2w(e.master_nick) + L")";
        else item += L"  (unido)";
        AppendMenuW(menu, MF_STRING, ID_HIST_BASE + i, item.c_str());
    }

    // Posicionar el menú debajo del botón ▼.
    RECT r;
    GetWindowRect(g_btn_hist, &r);
    int chosen = TrackPopupMenu(menu,
        TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
        r.left, r.bottom, 0, g_hwnd, nullptr);
    DestroyMenu(menu);

    if (chosen >= ID_HIST_BASE) {
        size_t idx = (size_t)(chosen - ID_HIST_BASE);
        if (idx < g_hist_snapshot.size()) {
            SetWindowTextW(g_edit_ip, s2w(g_hist_snapshot[idx].code).c_str());
        }
    }
}

void create_controls(HWND hwnd) {
    g_font_main = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_font_title = CreateFontW(-25, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_font_small = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_brush_bg = CreateSolidBrush(C_BG);
    g_brush_panel = CreateSolidBrush(C_PANEL);
    g_brush_field = CreateSolidBrush(C_FIELD);

    HFONT font = g_font_main;

    auto mk_label = [&](const wchar_t* text, int x, int y, int w, int h) -> HWND {
        HWND s = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                                 x, y, w, h, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(s, WM_SETFONT, (WPARAM)font, TRUE);
        return s;
    };
    auto mk_edit = [&](int id, const wchar_t* text, int x, int y, int w, int h) -> HWND {
        HWND e = CreateWindowExW(0, L"EDIT", text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            x, y, w, h, hwnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
        SendMessageW(e, WM_SETFONT, (WPARAM)font, TRUE);
        return e;
    };
    auto mk_btn = [&](int id, const wchar_t* text, int x, int y, int w, int h) -> HWND {
        HWND b = CreateWindowExW(0, L"BUTTON", text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            x, y, w, h, hwnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
        SendMessageW(b, WM_SETFONT, (WPARAM)font, TRUE);
        return b;
    };
    auto mk_combo = [&](int id, int x, int y, int w, int h) -> HWND {
        HWND c = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
            x, y, w, h, hwnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
        return c;
    };

    g_label_nick = mk_label(L"Nick", 34, 139, 90, 20);
    g_edit_nick = mk_edit(ID_NICK, L"user", 34, 163, 210, 30);

    g_label_code = mk_label(L"Código", 266, 139, 90, 20);
    g_edit_ip   = mk_edit(ID_IP, L"", 266, 163, 142, 30);
    g_btn_hist  = mk_btn(ID_BTN_HIST, L"v", 416, 163, 42, 30);
    g_btn_settings = mk_btn(ID_BTN_SETTINGS, L"⚙", 502, 32, 40, 34);

    g_btn_host  = mk_btn(ID_BTN_HOST,  L"Crear canal",  34,  207, 108, 32);
    g_btn_new   = mk_btn(ID_BTN_NEW,   L"Nuevo código", 150, 207, 118, 32);
    g_btn_join  = mk_btn(ID_BTN_JOIN,  L"Unirse",       276, 207, 76, 32);
    g_btn_leave = mk_btn(ID_BTN_LEAVE, L"",             360, 207, 42, 32);
    g_btn_mute  = mk_btn(ID_BTN_MUTE,  L"",             412, 207, 42, 32);
    g_btn_deafen = mk_btn(ID_BTN_DEAFEN, L"",           464, 207, 42, 32);

    g_capture = mk_combo(ID_CAPTURE, 88, 230, 202, 250);
    g_render = mk_combo(ID_RENDER, 320, 230, 170, 250);

    g_btn_set_mute = mk_btn(ID_BTN_SET_MUTE, L"Sin asignar", 260, 322, 132, 30);
    g_btn_clear_mute = mk_btn(ID_BTN_CLEAR_MUTE, L"X", 398, 322, 34, 30);
    g_btn_set_deafen = mk_btn(ID_BTN_SET_DEAFEN, L"Sin asignar", 260, 366, 132, 30);
    g_btn_clear_deafen = mk_btn(ID_BTN_CLEAR_DEAFEN, L"X", 398, 366, 34, 30);

    g_peers = CreateWindowExW(0, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
        34, 302, 508, 310, hwnd, (HMENU)(INT_PTR)ID_PEERS, nullptr, nullptr);
    SendMessageW(g_peers, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(g_peers, LB_SETITEMHEIGHT, 0, 34);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);
    g_status = CreateWindowExW(0, L"STATIC", nullptr,
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_STATUS, nullptr, nullptr);
    SendMessageW(g_status, WM_SETFONT, (WPARAM)g_font_small, TRUE);
    update_settings_panel();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_hwnd = hwnd;
            load_control_icons();
            create_controls(hwnd);
            // Cargar config: nick + último código hosteado en el campo.
            config_load(g_cfg);
            if (!g_cfg.nick.empty())
                SetWindowTextW(g_edit_nick, s2w(g_cfg.nick).c_str());
            if (!g_cfg.last_hosted_code.empty())
                SetWindowTextW(g_edit_ip, s2w(g_cfg.last_hosted_code).c_str());
            populate_device_combo(g_capture, g_capture_devices, true, g_cfg.capture_device_id);
            populate_device_combo(g_render, g_render_devices, false, g_cfg.render_device_id);
            register_hotkeys();
            refresh_hotkey_buttons();
            SetTimer(hwnd, ID_TIMER, 200, nullptr);
            update_buttons();
            refresh_status();
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            draw_shell(hwnd);
            return 0;

        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wp;
            SetBkMode(dc, TRANSPARENT);
            HWND ctl = (HWND)lp;
            if (ctl == g_edit_nick || ctl == g_edit_ip) {
                SetTextColor(dc, RGB(101, 115, 139));
                SetBkMode(dc, OPAQUE);
                SetBkColor(dc, C_FIELD);
                return (LRESULT)g_brush_field;
            }
            SetTextColor(dc, ctl == g_status ? C_ACCENT : C_MUTED);
            return (LRESULT)g_brush_panel;
        }

        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wp;
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, C_FIELD);
            SetTextColor(dc, C_TEXT);
            return (LRESULT)g_brush_field;
        }

        case WM_CTLCOLORLISTBOX: {
            HDC dc = (HDC)wp;
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, C_PANEL);
            SetTextColor(dc, C_TEXT);
            return (LRESULT)g_brush_panel;
        }

        case WM_DRAWITEM: {
            auto* dis = (DRAWITEMSTRUCT*)lp;
            if (!dis) return TRUE;
            if (dis->CtlID == ID_PEERS) {
                draw_peer_item(dis);
                return TRUE;
            }
            if ((dis->CtlID >= ID_BTN_HOST && dis->CtlID <= ID_BTN_NEW) ||
                (dis->CtlID >= ID_BTN_DEAFEN && dis->CtlID <= ID_BTN_SETTINGS)) {
                draw_button(dis);
                return TRUE;
            }
            break;
        }

        case WM_TIMER:
            if (wp == ID_TIMER) {
                if (g_client && g_client->failover_requested()) {
                    on_failover_to_master();
                }
                refresh_peers();
                refresh_status();
            }
            return 0;

        case WM_SIZE:
            MoveWindow(g_status, 0, HIWORD(lp) - 26, LOWORD(lp), 26, TRUE);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_COMMAND: {
            int id = LOWORD(wp);
            int code = HIWORD(wp);
            if ((id == ID_CAPTURE || id == ID_RENDER) && code == CBN_SELCHANGE) {
                save_ui_config();
                return 0;
            }
            if (code == BN_CLICKED) {
                switch (id) {
                    case ID_BTN_HOST:  on_host();      break;
                    case ID_BTN_NEW:   on_new_code();  break;
                    case ID_BTN_JOIN:  on_join();      break;
                    case ID_BTN_LEAVE: on_leave();     break;
                    case ID_BTN_MUTE:  on_mute();      break;
                    case ID_BTN_DEAFEN:on_deafen();    break;
                    case ID_BTN_HIST:  on_history();   break;
                    case ID_BTN_SETTINGS:
                        g_settings_open = !g_settings_open;
                        update_settings_panel();
                        update_buttons();
                        break;
                    case ID_BTN_SET_MUTE:
                        g_capture_hotkey = ID_HOTKEY_MUTE;
                        refresh_hotkey_buttons();
                        SetFocus(hwnd);
                        break;
                    case ID_BTN_SET_DEAFEN:
                        g_capture_hotkey = ID_HOTKEY_DEAFEN;
                        refresh_hotkey_buttons();
                        SetFocus(hwnd);
                        break;
                    case ID_BTN_CLEAR_MUTE:
                        g_cfg.mute_hotkey_mods = g_cfg.mute_hotkey_vk = 0;
                        save_hotkey_config();
                        register_hotkeys();
                        refresh_hotkey_buttons();
                        break;
                    case ID_BTN_CLEAR_DEAFEN:
                        g_cfg.deafen_hotkey_mods = g_cfg.deafen_hotkey_vk = 0;
                        save_hotkey_config();
                        register_hotkeys();
                        refresh_hotkey_buttons();
                        break;
                }
            }
            return 0;
        }

        case WM_HOTKEY:
            if (wp == ID_HOTKEY_MUTE) on_mute();
            else if (wp == ID_HOTKEY_DEAFEN) on_deafen();
            return 0;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (g_capture_hotkey) {
                uint32_t vk = (uint32_t)wp;
                if (vk != VK_CONTROL && vk != VK_SHIFT && vk != VK_MENU) {
                    uint32_t mods = 0;
                    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
                    if (GetKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;
                    if (GetKeyState(VK_MENU) & 0x8000) mods |= MOD_ALT;
                    if (g_capture_hotkey == ID_HOTKEY_MUTE) {
                        g_cfg.mute_hotkey_mods = mods;
                        g_cfg.mute_hotkey_vk = vk;
                    } else {
                        g_cfg.deafen_hotkey_mods = mods;
                        g_cfg.deafen_hotkey_vk = vk;
                    }
                    g_capture_hotkey = 0;
                    save_hotkey_config();
                    register_hotkeys();
                    refresh_hotkey_buttons();
                }
                return 0;
            }
            break;

        case WM_CLOSE:
            on_leave();
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, ID_TIMER);
            UnregisterHotKey(hwnd, ID_HOTKEY_MUTE);
            UnregisterHotKey(hwnd, ID_HOTKEY_DEAFEN);
            if (g_font_main) DeleteObject(g_font_main);
            if (g_font_title) DeleteObject(g_font_title);
            if (g_font_small) DeleteObject(g_font_small);
            if (g_brush_bg) DeleteObject(g_brush_bg);
            if (g_brush_panel) DeleteObject(g_brush_panel);
            if (g_brush_field) DeleteObject(g_brush_field);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

static LONG WINAPI top_level_filter(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    log_msg("CRASH SEH: code=0x%08lX addr=%p", (unsigned long)code, addr);
    char buf[256];
    snprintf(buf, sizeof(buf),
             "Crash SEH\nCódigo: 0x%08lX\nDirección: %p\n(ver fivecom.log)",
             (unsigned long)code, addr);
    MessageBoxA(nullptr, buf, "FiveCom crash", MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    log_init();
    log_msg("wWinMain: start");
    SetUnhandledExceptionFilter(top_level_filter);

    if (!wsa_init()) {
        MessageBoxW(nullptr, L"WSAStartup falló", L"FiveCom", MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_FIVECOM));
    wc.hIconSm       = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_FIVECOM));
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"FiveComWnd";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(
        0, L"FiveComWnd", L"FiveCom — Voice Chat",
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 580, 700,
        nullptr, nullptr, hInst, nullptr);

    if (!g_hwnd) {
        wsa_cleanup();
        return 1;
    }

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(g_hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    on_leave();
    wsa_cleanup();
    return (int)msg.wParam;
}
