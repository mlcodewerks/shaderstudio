#include "audio_decoder.h"
#include <formats/audio.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <memory>
#include <stdexcept>

std::vector<float> studio_decode_audio(const std::filesystem::path &path, unsigned output_rate)
{
    constexpr size_t limit = 256 * 1024 * 1024;
    auto size = std::filesystem::file_size(path);
    if (!size || size > 128 * 1024 * 1024)
        throw std::runtime_error("Audio files must be nonempty and at most 128 MiB.");
    std::vector<unsigned char> bytes(static_cast<size_t>(size));
    std::ifstream input(path, std::ios::binary);
    if (!input.read(reinterpret_cast<char *>(bytes.data()), size))
        throw std::runtime_error("Cannot read audio file.");
    auto type = audio_transfer_ogg_audio_type(bytes.data(), bytes.size());
    if (type == AUDIO_TYPE_NONE)
        type = audio_transfer_webm_audio_type(bytes.data(), bytes.size());
    if (type == AUDIO_TYPE_NONE)
        type = audio_decode_get_type(path.u8string().c_str());
    auto ext = path.extension().u8string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    if (type == AUDIO_TYPE_NONE && (ext == ".aac" || ext == ".m4a" || ext == ".mp4")) type = AUDIO_TYPE_AAC;
    auto release = [type](void *p) { audio_transfer_free(p, type); };
    std::unique_ptr<void, decltype(release)> decoder(audio_transfer_new(type), release);
    if (!decoder) throw std::runtime_error("Unsupported audio format.");
    audio_transfer_set_buffer_ptr(decoder.get(), type, bytes.data(), bytes.size());
    audio_transfer_set_output_rate(decoder.get(), type, output_rate);
    if (!audio_transfer_start(decoder.get(), type)) throw std::runtime_error("Cannot decode audio file.");
    unsigned channels = 0, rate = 0;
    if (!audio_transfer_info(decoder.get(), type, &channels, &rate, nullptr) ||
        !channels || channels > 8 || !rate || rate > 384000 || !output_rate || output_rate > 384000)
        throw std::runtime_error("Unsupported audio channel count or sample rate.");
    SDL_AudioSpec src{SDL_AUDIO_F32, static_cast<int>(channels), static_cast<int>(rate)};
    SDL_AudioSpec dst{SDL_AUDIO_F32, 2, static_cast<int>(output_rate)};
    std::unique_ptr<SDL_AudioStream, decltype(&SDL_DestroyAudioStream)> stream(
        SDL_CreateAudioStream(&src, &dst), SDL_DestroyAudioStream);
    if (!stream) throw std::runtime_error(SDL_GetError());
    std::vector<float> result, chunk(4096 * channels);
    auto drain = [&]() {
        int available = SDL_GetAudioStreamAvailable(stream.get());
        if (available < 0) throw std::runtime_error(SDL_GetError());
        if (result.size() * sizeof(float) + available > limit)
            throw std::runtime_error("Decoded audio exceeds 256 MiB. Use a shorter clip.");
        size_t old = result.size();
        result.resize(old + available / sizeof(float));
        if (available && SDL_GetAudioStreamData(stream.get(), result.data() + old, available) != available)
            throw std::runtime_error("Cannot convert decoded audio.");
    };
    for (;;)
    {
        size_t frames = 0;
        int code = audio_transfer_read_f32(decoder.get(), type, chunk.data(), 4096, &frames);
        if (code < 0 || (!frames && code != AUDIO_PROCESS_END))
            throw std::runtime_error("Audio decoding failed or stalled.");
        if (frames && !SDL_PutAudioStreamData(stream.get(), chunk.data(), static_cast<int>(frames * channels * sizeof(float))))
            throw std::runtime_error(SDL_GetError());
        drain();
        if (code == AUDIO_PROCESS_END) break;
    }
    if (!SDL_FlushAudioStream(stream.get())) throw std::runtime_error(SDL_GetError());
    drain();
    if (result.size() < 2) throw std::runtime_error("Audio file contains no samples.");
    for (float &sample : result)
        sample = std::isfinite(sample) ? std::clamp(sample, -1.f, 1.f) : 0.f;
    return result;
}
