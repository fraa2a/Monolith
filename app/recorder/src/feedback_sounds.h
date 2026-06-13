#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#include <cmath>
#include <cstdint>
#include <vector>

// Synthesised WAV feedback tones played on record start/stop and clip save.
// All sounds are sine-wave based with a short attack/decay envelope so they
// feel intentional rather than system-alert-like.
namespace feedback {

enum class Sound { RecordStart, RecordStop, ClipSaved };

namespace detail {

static constexpr int    kSampleRate = 44100;
static constexpr double kPi         = 3.14159265358979323846;

static void push_u16(std::vector<uint8_t>& b, uint16_t v)
{
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
}
static void push_u32(std::vector<uint8_t>& b, uint32_t v)
{
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v >> 16));
    b.push_back(static_cast<uint8_t>(v >> 24));
}

struct Tone { double freq; double ms; double amp; };

static std::vector<uint8_t> make_wav(const std::vector<Tone>& tones)
{
    const int attack_s = static_cast<int>(0.009 * kSampleRate);
    const int decay_s  = static_cast<int>(0.013 * kSampleRate);

    std::vector<int16_t> pcm;
    for (const auto& t : tones) {
        int n = static_cast<int>(t.ms * kSampleRate / 1000.0);
        for (int i = 0; i < n; ++i) {
            double env = 1.0;
            if (i < attack_s)
                env = static_cast<double>(i) / attack_s;
            else if (i >= n - decay_s)
                env = static_cast<double>(n - i) / decay_s;
            double s = t.amp * env * std::sin(2.0 * kPi * t.freq * i / kSampleRate);
            pcm.push_back(static_cast<int16_t>(s * 32767.0));
        }
    }

    uint32_t data_sz = static_cast<uint32_t>(pcm.size() * 2);
    std::vector<uint8_t> wav;
    wav.reserve(44 + data_sz);

    // RIFF / WAVE header
    for (char c : {'R','I','F','F'}) wav.push_back(static_cast<uint8_t>(c));
    push_u32(wav, 36 + data_sz);
    for (char c : {'W','A','V','E'}) wav.push_back(static_cast<uint8_t>(c));

    // fmt chunk
    for (char c : {'f','m','t',' '}) wav.push_back(static_cast<uint8_t>(c));
    push_u32(wav, 16);
    push_u16(wav, 1);                      // PCM
    push_u16(wav, 1);                      // mono
    push_u32(wav, kSampleRate);
    push_u32(wav, kSampleRate * 2);        // byte rate
    push_u16(wav, 2);                      // block align
    push_u16(wav, 16);                     // bits per sample

    // data chunk
    for (char c : {'d','a','t','a'}) wav.push_back(static_cast<uint8_t>(c));
    push_u32(wav, data_sz);
    for (int16_t s : pcm) {
        wav.push_back(static_cast<uint8_t>(s));
        wav.push_back(static_cast<uint8_t>(static_cast<uint16_t>(s) >> 8));
    }
    return wav;
}

static std::vector<uint8_t> g_wav_start;
static std::vector<uint8_t> g_wav_stop;
static std::vector<uint8_t> g_wav_clip;

static void init()
{
    if (!g_wav_start.empty()) return;

    // Start: C5 → G5 ascending — "recording on"
    g_wav_start = make_wav({
        {523.25, 65.0,  0.40},
        {783.99, 105.0, 0.48},
    });

    // Stop: G5 → C5 descending — "recording off"
    g_wav_stop = make_wav({
        {783.99, 65.0,  0.48},
        {523.25, 105.0, 0.38},
    });

    // Clip saved: C5 → E5 → G5 arpeggio — "saved!"
    g_wav_clip = make_wav({
        {523.25, 50.0, 0.38},
        {659.25, 50.0, 0.42},
        {783.99, 85.0, 0.48},
    });
}

} // namespace detail

static void play(Sound s)
{
    detail::init();

    const std::vector<uint8_t>* buf = nullptr;
    switch (s) {
    case Sound::RecordStart: buf = &detail::g_wav_start; break;
    case Sound::RecordStop:  buf = &detail::g_wav_stop;  break;
    case Sound::ClipSaved:   buf = &detail::g_wav_clip;  break;
    }
    if (!buf || buf->empty()) return;

    PlaySoundW(reinterpret_cast<LPCWSTR>(buf->data()), nullptr,
               SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

} // namespace feedback
