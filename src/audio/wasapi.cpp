#include "audio/wasapi.h"
#include "core/log.h"

#define INITGUID
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propsys.h>
#include <avrt.h>
#include <ks.h>
#include <ksmedia.h>

#include <vector>
#include <algorithm>
#include <cstring>

namespace fivecom {

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM      0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

static const CLSID CLSID_MMDeviceEnumerator_local = __uuidof(MMDeviceEnumerator);
static const IID   IID_IMMDeviceEnumerator_local  = __uuidof(IMMDeviceEnumerator);
static const IID   IID_IAudioClient_local         = __uuidof(IAudioClient);
static const IID   IID_IAudioCaptureClient_local  = __uuidof(IAudioCaptureClient);
static const IID   IID_IAudioRenderClient_local   = __uuidof(IAudioRenderClient);

struct ComScope {
    bool ok;
    ComScope() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ok = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    }
    ~ComScope() { if (ok) CoUninitialize(); }
};

template <class T> static void safe_release(T*& p) { if (p) { p->Release(); p = nullptr; } }

static std::string hr_str(const char* what, HRESULT hr) {
    char b[160];
    snprintf(b, sizeof(b), "%s (HRESULT 0x%08lX)", what, (unsigned long)hr);
    return std::string(b);
}

static std::wstring s2w_local(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    if (!w.empty() && w.back() == 0) w.pop_back();
    return w;
}

static std::string w2s_local(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == 0) s.pop_back();
    return s;
}

static HRESULT open_audio_device(IMMDeviceEnumerator* enumr, EDataFlow flow,
                                 const std::string& id, IMMDevice** out) {
    *out = nullptr;
    if (!id.empty()) {
        std::wstring wid = s2w_local(id);
        HRESULT hr = enumr->GetDevice(wid.c_str(), out);
        if (SUCCEEDED(hr)) return hr;
        log_msg("WASAPI: selected device unavailable, falling back to default hr=0x%08lX",
                (unsigned long)hr);
    }
    // Usar eConsole evita que Windows active el "ducking" de comunicaciones,
    // que baja el volumen del resto de aplicaciones al iniciar FiveCom.
    HRESULT hr = enumr->GetDefaultAudioEndpoint(flow, eConsole, out);
    if (FAILED(hr)) hr = enumr->GetDefaultAudioEndpoint(flow, eCommunications, out);
    return hr;
}

std::vector<AudioDeviceInfo> list_audio_devices(bool capture) {
    std::vector<AudioDeviceInfo> out;
    ComScope com;
    if (!com.ok) return out;

    IMMDeviceEnumerator* enumr = nullptr;
    IMMDeviceCollection* coll = nullptr;
    IMMDevice* def = nullptr;
    LPWSTR def_id = nullptr;
    EDataFlow flow = capture ? eCapture : eRender;

    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator_local, nullptr, CLSCTX_ALL,
                                  IID_IMMDeviceEnumerator_local, (void**)&enumr);
    if (FAILED(hr)) return out;

    if (SUCCEEDED(open_audio_device(enumr, flow, "", &def))) def->GetId(&def_id);

    hr = enumr->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &coll);
    if (SUCCEEDED(hr)) {
        UINT count = 0;
        coll->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* dev = nullptr;
            if (FAILED(coll->Item(i, &dev)) || !dev) continue;
            LPWSTR idw = nullptr;
            IPropertyStore* props = nullptr;
            std::wstring name = L"Dispositivo de audio";
            if (SUCCEEDED(dev->GetId(&idw)) && SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
                    name = pv.pwszVal;
                }
                PropVariantClear(&pv);
            }
            if (idw) {
                AudioDeviceInfo info;
                info.id = w2s_local(idw);
                info.name = w2s_local(name);
                info.is_default = def_id && wcscmp(idw, def_id) == 0;
                out.push_back(std::move(info));
                CoTaskMemFree(idw);
            }
            safe_release(props);
            safe_release(dev);
        }
    }

    if (def_id) CoTaskMemFree(def_id);
    safe_release(def);
    safe_release(coll);
    safe_release(enumr);
    return out;
}

// Build the format we always want to talk to the audio engine in.
static void build_target_format(WAVEFORMATEX& w) {
    std::memset(&w, 0, sizeof(w));
    w.wFormatTag      = WAVE_FORMAT_PCM;
    w.nChannels       = (WORD)CHANNELS;
    w.nSamplesPerSec  = SAMPLE_RATE;
    w.wBitsPerSample  = 16;
    w.nBlockAlign     = w.nChannels * w.wBitsPerSample / 8;
    w.nAvgBytesPerSec = w.nSamplesPerSec * w.nBlockAlign;
    w.cbSize          = 0;
}

// =========================================================================
// CAPTURE
// =========================================================================
std::string WasapiCapture::last_error() { return runtime_err_; }

bool WasapiCapture::start(Callback cb, std::string& err) {
    log_msg("WasapiCapture::start enter");
    if (running_.load()) { err = "already running"; return false; }
    cb_ = std::move(cb);
    init_ok_ = false;
    init_err_.clear();
    runtime_err_.clear();
    init_evt_ = (void*)CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!init_evt_) { err = "CreateEvent failed"; return false; }

    running_.store(true);
    thread_ = std::thread([this]() { run(); });

    log_msg("WasapiCapture::start waiting for init event...");
    DWORD wr = WaitForSingleObject((HANDLE)init_evt_, 5000);
    log_msg("WasapiCapture::start wait result=%lu init_ok=%d err='%s'",
            (unsigned long)wr, (int)init_ok_, init_err_.c_str());
    CloseHandle((HANDLE)init_evt_);
    init_evt_ = nullptr;

    if (!init_ok_) {
        running_.store(false);
        if (thread_.joinable()) thread_.join();
        err = init_err_.empty() ? "captura: init falló (timeout?)" : init_err_;
        return false;
    }
    err.clear();
    return true;
}

void WasapiCapture::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

void WasapiCapture::run() {
    log_msg("CAPTURE: run() entered");
    ComScope com;
    HRESULT hr = S_OK;

    IMMDeviceEnumerator* enumr  = nullptr;
    IMMDevice*           dev    = nullptr;
    IAudioClient*        client = nullptr;
    IAudioCaptureClient* cap    = nullptr;
    HANDLE               evt    = nullptr;
    HANDLE               mmcss  = nullptr;
    bool                 started = false;

    auto signal_init = [&](bool ok, const std::string& err) {
        log_msg("CAPTURE: signal_init ok=%d err='%s'", (int)ok, err.c_str());
        init_ok_  = ok;
        init_err_ = err;
        if (init_evt_) SetEvent((HANDLE)init_evt_);
    };

    auto cleanup = [&]() {
        if (started && client) client->Stop();
        safe_release(cap);
        safe_release(client);
        safe_release(dev);
        safe_release(enumr);
        if (evt)   { CloseHandle(evt); evt = nullptr; }
        if (mmcss) { AvRevertMmThreadCharacteristics(mmcss); mmcss = nullptr; }
    };

    if (!com.ok) { signal_init(false, "CoInitializeEx falló"); return; }

    hr = CoCreateInstance(CLSID_MMDeviceEnumerator_local, nullptr, CLSCTX_ALL,
                          IID_IMMDeviceEnumerator_local, (void**)&enumr);
    if (FAILED(hr)) { signal_init(false, hr_str("MMDeviceEnumerator", hr)); cleanup(); return; }

    hr = open_audio_device(enumr, eCapture, device_id_, &dev);
    if (FAILED(hr)) { signal_init(false, hr_str("no hay micrófono default", hr)); cleanup(); return; }

    hr = dev->Activate(IID_IAudioClient_local, CLSCTX_ALL, nullptr, (void**)&client);
    if (FAILED(hr)) { signal_init(false, hr_str("Activate IAudioClient", hr)); cleanup(); return; }

    WAVEFORMATEX want;
    build_target_format(want);

    REFERENCE_TIME def_period = 0, min_period = 0;
    client->GetDevicePeriod(&def_period, &min_period);

    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    log_msg("CAPTURE: Initialize trying with AUTOCONVERTPCM");
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                            def_period, 0, &want, nullptr);
    log_msg("CAPTURE: first Initialize hr=0x%08lX", (unsigned long)hr);
    if (FAILED(hr)) {
        // Fallback: cualquier error -> reintentar sin AUTOCONVERTPCM
        // (algunas combinaciones rechazan int16 mono explícito)
        log_msg("CAPTURE: retry Initialize without AUTOCONVERTPCM");
        // Necesitamos un IAudioClient nuevo: el viejo quedó inicializado fallido.
        IAudioClient* c2 = nullptr;
        if (SUCCEEDED(dev->Activate(IID_IAudioClient_local, CLSCTX_ALL, nullptr, (void**)&c2))) {
            safe_release(client);
            client = c2;
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                    def_period, 0, &want, nullptr);
            log_msg("CAPTURE: fallback Initialize hr=0x%08lX", (unsigned long)hr);
        }
    }
    if (FAILED(hr)) { signal_init(false, hr_str("captura Initialize", hr)); cleanup(); return; }

    evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!evt) { signal_init(false, "CreateEvent (capture)"); cleanup(); return; }
    hr = client->SetEventHandle(evt);
    if (FAILED(hr)) { signal_init(false, hr_str("SetEventHandle", hr)); cleanup(); return; }

    hr = client->GetService(IID_IAudioCaptureClient_local, (void**)&cap);
    if (FAILED(hr)) { signal_init(false, hr_str("GetService capture", hr)); cleanup(); return; }

    hr = client->Start();
    if (FAILED(hr)) { signal_init(false, hr_str("captura Start", hr)); cleanup(); return; }
    started = true;
    log_msg("CAPTURE: started OK");

    DWORD task_idx = 0;
    mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_idx);

    signal_init(true, "");

    std::vector<int16_t> accum;
    accum.reserve(FRAME_SAMPLES * 4);

    while (running_.load()) {
        DWORD wr = WaitForSingleObject(evt, 200);
        if (wr == WAIT_TIMEOUT) continue;
        if (wr != WAIT_OBJECT_0) break;

        UINT32 packet_size = 0;
        cap->GetNextPacketSize(&packet_size);
        while (packet_size > 0 && running_.load()) {
            BYTE*  data = nullptr;
            UINT32 frames = 0;
            DWORD  pkt_flags = 0;
            hr = cap->GetBuffer(&data, &frames, &pkt_flags, nullptr, nullptr);
            if (FAILED(hr)) break;

            size_t old = accum.size();
            accum.resize(old + frames);
            if ((pkt_flags & AUDCLNT_BUFFERFLAGS_SILENT) || data == nullptr) {
                std::fill(accum.begin() + old, accum.end(), (int16_t)0);
            } else {
                std::memcpy(accum.data() + old, data, frames * sizeof(int16_t));
            }
            cap->ReleaseBuffer(frames);

            while (accum.size() >= FRAME_SAMPLES) {
                cb_(accum.data(), FRAME_SAMPLES);
                accum.erase(accum.begin(), accum.begin() + FRAME_SAMPLES);
            }
            cap->GetNextPacketSize(&packet_size);
        }
    }

    cleanup();
}

// =========================================================================
// RENDER
// =========================================================================
std::string WasapiRender::last_error() { return runtime_err_; }

bool WasapiRender::start(Callback cb, std::string& err) {
    log_msg("WasapiRender::start enter");
    if (running_.load()) { err = "already running"; return false; }
    cb_ = std::move(cb);
    init_ok_ = false;
    init_err_.clear();
    runtime_err_.clear();
    init_evt_ = (void*)CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!init_evt_) { err = "CreateEvent failed"; return false; }

    running_.store(true);
    thread_ = std::thread([this]() { run(); });

    log_msg("WasapiRender::start waiting for init event...");
    DWORD wr = WaitForSingleObject((HANDLE)init_evt_, 5000);
    log_msg("WasapiRender::start wait result=%lu init_ok=%d err='%s'",
            (unsigned long)wr, (int)init_ok_, init_err_.c_str());
    CloseHandle((HANDLE)init_evt_);
    init_evt_ = nullptr;

    if (!init_ok_) {
        running_.store(false);
        if (thread_.joinable()) thread_.join();
        err = init_err_.empty() ? "render: init falló (timeout?)" : init_err_;
        return false;
    }
    err.clear();
    return true;
}

void WasapiRender::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

void WasapiRender::run() {
    log_msg("RENDER: run() entered");
    ComScope com;
    HRESULT hr = S_OK;

    IMMDeviceEnumerator* enumr  = nullptr;
    IMMDevice*           dev    = nullptr;
    IAudioClient*        client = nullptr;
    IAudioRenderClient*  ren    = nullptr;
    HANDLE               evt    = nullptr;
    HANDLE               mmcss  = nullptr;
    bool                 started = false;
    UINT32               buffer_frames = 0;

    auto signal_init = [&](bool ok, const std::string& err) {
        log_msg("RENDER: signal_init ok=%d err='%s'", (int)ok, err.c_str());
        init_ok_  = ok;
        init_err_ = err;
        if (init_evt_) SetEvent((HANDLE)init_evt_);
    };

    auto cleanup = [&]() {
        if (started && client) client->Stop();
        safe_release(ren);
        safe_release(client);
        safe_release(dev);
        safe_release(enumr);
        if (evt)   { CloseHandle(evt); evt = nullptr; }
        if (mmcss) { AvRevertMmThreadCharacteristics(mmcss); mmcss = nullptr; }
    };

    if (!com.ok) { signal_init(false, "CoInitializeEx falló"); return; }

    hr = CoCreateInstance(CLSID_MMDeviceEnumerator_local, nullptr, CLSCTX_ALL,
                          IID_IMMDeviceEnumerator_local, (void**)&enumr);
    if (FAILED(hr)) { signal_init(false, hr_str("MMDeviceEnumerator", hr)); cleanup(); return; }

    hr = open_audio_device(enumr, eRender, device_id_, &dev);
    if (FAILED(hr)) { signal_init(false, hr_str("no hay parlantes default", hr)); cleanup(); return; }

    hr = dev->Activate(IID_IAudioClient_local, CLSCTX_ALL, nullptr, (void**)&client);
    if (FAILED(hr)) { signal_init(false, hr_str("Activate IAudioClient", hr)); cleanup(); return; }

    WAVEFORMATEX want;
    build_target_format(want);

    REFERENCE_TIME def_period = 0, min_period = 0;
    client->GetDevicePeriod(&def_period, &min_period);

    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    log_msg("RENDER: Initialize trying with AUTOCONVERTPCM");
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                            def_period, 0, &want, nullptr);
    log_msg("RENDER: first Initialize hr=0x%08lX", (unsigned long)hr);
    if (FAILED(hr)) {
        log_msg("RENDER: retry Initialize without AUTOCONVERTPCM");
        IAudioClient* c2 = nullptr;
        if (SUCCEEDED(dev->Activate(IID_IAudioClient_local, CLSCTX_ALL, nullptr, (void**)&c2))) {
            safe_release(client);
            client = c2;
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                    def_period, 0, &want, nullptr);
            log_msg("RENDER: fallback Initialize hr=0x%08lX", (unsigned long)hr);
        }
    }
    if (FAILED(hr)) { signal_init(false, hr_str("render Initialize", hr)); cleanup(); return; }

    evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!evt) { signal_init(false, "CreateEvent (render)"); cleanup(); return; }
    client->SetEventHandle(evt);

    client->GetBufferSize(&buffer_frames);
    log_msg("RENDER: buffer_frames=%lu", (unsigned long)buffer_frames);

    hr = client->GetService(IID_IAudioRenderClient_local, (void**)&ren);
    if (FAILED(hr)) { signal_init(false, hr_str("GetService render", hr)); cleanup(); return; }

    // Pre-fill con silencio para arrancar limpio.
    {
        BYTE* data = nullptr;
        if (SUCCEEDED(ren->GetBuffer(buffer_frames, &data))) {
            ren->ReleaseBuffer(buffer_frames, AUDCLNT_BUFFERFLAGS_SILENT);
        }
    }

    hr = client->Start();
    if (FAILED(hr)) { signal_init(false, hr_str("render Start", hr)); cleanup(); return; }
    started = true;
    log_msg("RENDER: started OK");

    DWORD task_idx = 0;
    mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_idx);

    signal_init(true, "");

    // Buffer "stage" para amortiguar entre el callback (que entrega siempre
    // FRAME_SAMPLES por llamada) y WASAPI (que pide tamaños arbitrarios).
    std::vector<int16_t> stage;
    stage.reserve(FRAME_SAMPLES * 4);
    std::vector<int16_t> work(FRAME_SAMPLES);

    while (running_.load()) {
        DWORD wr = WaitForSingleObject(evt, 200);
        if (wr == WAIT_TIMEOUT) continue;
        if (wr != WAIT_OBJECT_0) break;

        UINT32 padding = 0;
        client->GetCurrentPadding(&padding);
        UINT32 to_fill = buffer_frames - padding;
        if (to_fill == 0) continue;

        // Asegurar que tenemos al menos `to_fill` muestras en el stage.
        while (stage.size() < to_fill && running_.load()) {
            bool keep = cb_(work.data(), FRAME_SAMPLES);
            if (!keep) { running_.store(false); break; }
            stage.insert(stage.end(), work.begin(), work.end());
        }
        if (!running_.load()) break;

        BYTE* data = nullptr;
        hr = ren->GetBuffer(to_fill, &data);
        if (FAILED(hr)) continue;

        std::memcpy(data, stage.data(), to_fill * sizeof(int16_t));
        stage.erase(stage.begin(), stage.begin() + to_fill);
        ren->ReleaseBuffer(to_fill, 0);
    }

    cleanup();
}

} // namespace fivecom
