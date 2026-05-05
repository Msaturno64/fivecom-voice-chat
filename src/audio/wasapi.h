#pragma once
#include <cstdint>
#include <atomic>
#include <thread>
#include <functional>
#include <string>
#include <vector>

namespace fivecom {

constexpr uint32_t SAMPLE_RATE   = 48000;
constexpr uint32_t CHANNELS      = 1;
constexpr uint32_t FRAME_MS      = 20;
constexpr uint32_t FRAME_SAMPLES = SAMPLE_RATE * FRAME_MS / 1000; // 960

struct AudioDeviceInfo {
    std::string id;
    std::string name;
    bool is_default = false;
};

std::vector<AudioDeviceInfo> list_audio_devices(bool capture);

class WasapiCapture {
public:
    using Callback = std::function<void(const int16_t* pcm, size_t samples)>;

    bool start(Callback cb, std::string& err);
    void stop();
    bool running() const { return running_.load(); }
    void set_device_id(std::string id) { device_id_ = std::move(id); }

    // Last error reported by worker thread (after start succeeded but loop died).
    std::string last_error();

private:
    void run();

    Callback cb_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    // init handshake
    void* init_evt_ = nullptr;
    bool        init_ok_ = false;
    std::string init_err_;
    std::string runtime_err_;
    std::string device_id_;
};

class WasapiRender {
public:
    using Callback = std::function<bool(int16_t* pcm, size_t samples)>;

    bool start(Callback cb, std::string& err);
    void stop();
    bool running() const { return running_.load(); }
    void set_device_id(std::string id) { device_id_ = std::move(id); }

    std::string last_error();

private:
    void run();

    Callback cb_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    void* init_evt_ = nullptr;
    bool        init_ok_ = false;
    std::string init_err_;
    std::string runtime_err_;
    std::string device_id_;
};

} // namespace fivecom
