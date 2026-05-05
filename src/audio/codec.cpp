#include "audio/codec.h"
#include <cstring>

namespace fivecom {

bool OpusEncoderW::init(std::string& err) {
    int e = 0;
    enc_ = opus_encoder_create(SAMPLE_RATE, CHANNELS, OPUS_APPLICATION_VOIP, &e);
    if (e != OPUS_OK || !enc_) {
        err = "opus_encoder_create failed";
        return false;
    }
    opus_encoder_ctl(enc_, OPUS_SET_BITRATE(OPUS_BITRATE));
    opus_encoder_ctl(enc_, OPUS_SET_VBR(1));
    opus_encoder_ctl(enc_, OPUS_SET_DTX(1));
    opus_encoder_ctl(enc_, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(enc_, OPUS_SET_PACKET_LOSS_PERC(5));
    opus_encoder_ctl(enc_, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(enc_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    return true;
}

OpusEncoderW::~OpusEncoderW() {
    if (enc_) { opus_encoder_destroy(enc_); enc_ = nullptr; }
}

int OpusEncoderW::encode(const int16_t* pcm, uint8_t* out, size_t out_max) {
    if (!enc_) return -1;
    int n = opus_encode(enc_, pcm, FRAME_SAMPLES, out, (opus_int32)out_max);
    return n;
}

bool OpusDecoderW::init(std::string& err) {
    int e = 0;
    dec_ = opus_decoder_create(SAMPLE_RATE, CHANNELS, &e);
    if (e != OPUS_OK || !dec_) {
        err = "opus_decoder_create failed";
        return false;
    }
    return true;
}

OpusDecoderW::~OpusDecoderW() {
    if (dec_) { opus_decoder_destroy(dec_); dec_ = nullptr; }
}

int OpusDecoderW::decode(const uint8_t* data, size_t len, int16_t* pcm_out) {
    if (!dec_) return 0;
    if (data && len > 0) {
        return opus_decode(dec_, data, (opus_int32)len, pcm_out, FRAME_SAMPLES, 0);
    }
    return opus_decode(dec_, nullptr, 0, pcm_out, FRAME_SAMPLES, 0);
}

// ------------- JitterBuffer -------------
JitterBuffer::JitterBuffer(size_t target_frames, size_t max_frames)
    : target_(target_frames), maxv_(max_frames) {}

void JitterBuffer::push(const int16_t* pcm, size_t samples) {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<int16_t> v(pcm, pcm + samples);
    q_.push_back(std::move(v));
    while (q_.size() > maxv_) q_.pop_front(); // descartar el más viejo
}

void JitterBuffer::pop(int16_t* out) {
    std::lock_guard<std::mutex> lk(mu_);

    if (!primed_) {
        if (q_.size() < target_) {
            std::memset(out, 0, FRAME_SAMPLES * sizeof(int16_t));
            return;
        }
        primed_ = true;
    }

    if (q_.empty()) {
        std::memset(out, 0, FRAME_SAMPLES * sizeof(int16_t));
        primed_ = false; // re-prime la próxima vez
        return;
    }
    auto& f = q_.front();
    size_t n = (f.size() < FRAME_SAMPLES) ? f.size() : FRAME_SAMPLES;
    std::memcpy(out, f.data(), n * sizeof(int16_t));
    if (n < FRAME_SAMPLES) {
        std::memset(out + n, 0, (FRAME_SAMPLES - n) * sizeof(int16_t));
    }
    q_.pop_front();
}

size_t JitterBuffer::size() {
    std::lock_guard<std::mutex> lk(mu_);
    return q_.size();
}

void JitterBuffer::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    q_.clear();
    primed_ = false;
}

} // namespace fivecom
