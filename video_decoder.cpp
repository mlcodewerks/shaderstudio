#include "video_decoder.h"
#include <formats/rmp4_video.h>
#include <formats/rwebm_video.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>

struct studio_video_decoder::impl
{
    std::vector<unsigned char> bytes;
    rmp4_video_stream_t *mp4 = nullptr;
    rwebm_video_stream_t *webm = nullptr;
    unsigned width = 0, height = 0;
    double next_time = 0, last_time = -1;
    ~impl()
    {
        if (mp4) rmp4_video_stream_close(mp4);
        if (webm) rwebm_video_stream_close(webm);
    }
    void rewind()
    {
        if (mp4) rmp4_video_stream_rewind(mp4);
        if (webm) rwebm_video_stream_rewind(webm);
    }
    const uint32_t *next(int &duration)
    {
        return mp4 ? rmp4_video_stream_next(mp4, &duration) : rwebm_video_stream_next(webm, &duration);
    }
};

studio_video_decoder::studio_video_decoder() = default;
studio_video_decoder::~studio_video_decoder() = default;
void studio_video_decoder::close() { state.reset(); }
void studio_video_decoder::restart()
{
    if (!state) return;
    state->rewind();
    state->next_time = 0;
    state->last_time = -1;
}
void studio_video_decoder::open(const std::filesystem::path &path)
{
    auto candidate = std::make_unique<impl>();
    const auto size = std::filesystem::file_size(path);
    if (!size || size > 256 * 1024 * 1024)
        throw std::runtime_error("Video files must be nonempty and at most 256 MiB.");
    candidate->bytes.resize(static_cast<size_t>(size));
    std::ifstream input(path, std::ios::binary);
    if (!input.read(reinterpret_cast<char *>(candidate->bytes.data()), size))
        throw std::runtime_error("Cannot read video file.");
    auto &s = *candidate;
    // Probe the container rather than relying on the filename extension.
    s.mp4 = rmp4_video_stream_open(s.bytes.data(), s.bytes.size());
    if (s.mp4)
        rmp4_video_stream_get_info(s.mp4, &s.width, &s.height, nullptr, nullptr);
    else
    {
        s.webm = rwebm_video_stream_open(s.bytes.data(), s.bytes.size());
        if (!s.webm)
            throw std::runtime_error("Unsupported video. Use MP4 (H.264/H.265/VP8/VP9) or WebM (VP8/VP9).");
        rwebm_video_stream_get_info(s.webm, &s.width, &s.height, nullptr, nullptr);
    }
    if (!s.width || !s.height || s.width > 4096 || s.height > 4096)
        throw std::runtime_error("Video frames must be no larger than 4096 pixels per side.");
    state = std::move(candidate);
}
bool studio_video_decoder::update(double seconds, studio_video_frame &frame, double fallback_fps)
{
    if (!state) return false;
    if (!std::isfinite(seconds)) throw std::runtime_error("Invalid video playback time.");
    seconds = std::max(0.0, seconds);
    if (seconds < state->last_time) restart();
    auto &s = *state;
    s.last_time = seconds;
    bool changed = false;
    // Bound catch-up work when rendering falls behind. Pause keeps the same frame.
    if (!std::isfinite(fallback_fps)) fallback_fps = 30;
    for (unsigned decoded = 0; seconds >= s.next_time && decoded < 8; ++decoded)
    {
        int duration = 0;
        const uint32_t *pixels = s.next(duration);
        if (!pixels)
        {
            s.rewind();
            pixels = s.next(duration);
            if (!pixels) throw std::runtime_error("Cannot decode a video frame.");
        }
        frame.w = static_cast<int>(s.width);
        frame.h = static_cast<int>(s.height);
        const auto *rgba = reinterpret_cast<const unsigned char *>(pixels);
        frame.pixels.assign(rgba, rgba + static_cast<size_t>(s.width) * s.height * 4);
        s.next_time += duration > 0 ? duration / 1000.0 : 1.0 / std::clamp(fallback_fps, 1.0, 240.0);
        changed = true;
    }
    return changed;
}
