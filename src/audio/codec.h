#pragma once
#include "audio/wasapi.h"

#include <opus/opus.h>
#include <cstdint>
#include <vector>
#include <deque>
#include <mutex>

namespace fivecom {

constexpr int OPUS_BITRATE  = 24000; // 24 kbps voz
constexpr int MAX_OPUS_PKT  = 1275;  // tope de payload Opus

class OpusEncoderW {
public:
    bool init(std::string& err);
    ~OpusEncoderW();

    // pcm: 960 muestras int16 mono. Devuelve bytes escritos en out (puede ser
    // 1 o 2 bytes si DTX detecta silencio).
    int encode(const int16_t* pcm, uint8_t* out, size_t out_max);

private:
    OpusEncoder* enc_ = nullptr;
};

class OpusDecoderW {
public:
    bool init(std::string& err);
    ~OpusDecoderW();

    // pcm_out: buffer para 960 muestras. Si data==nullptr o len==0, decodifica
    // como pérdida (PLC). Devuelve número de muestras decodificadas.
    int decode(const uint8_t* data, size_t len, int16_t* pcm_out);

private:
    OpusDecoder* dec_ = nullptr;
};

// Buffer de jitter por-peer: cola corta de frames decodificados (PCM).
// Mantiene un nivel objetivo en frames; descarta el más viejo si crece demasiado,
// inserta silencio si está vacío.
class JitterBuffer {
public:
    explicit JitterBuffer(size_t target_frames = 2, size_t max_frames = 6);

    // Agrega un frame de PCM (960 muestras). Lo decodifica el caller.
    void push(const int16_t* pcm, size_t samples);

    // Llena out con FRAME_SAMPLES muestras. Si está vacío -> silencio.
    void pop(int16_t* out);

    size_t size();
    void   clear();

private:
    std::deque<std::vector<int16_t>> q_;
    std::mutex mu_;
    size_t target_;
    size_t maxv_;
    bool   primed_ = false;
};

} // namespace fivecom
