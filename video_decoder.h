#pragma once
#include <filesystem>
#include <memory>
#include <vector>

struct studio_video_frame
{
    int w = 0, h = 0;
    std::vector<unsigned char> pixels;
};

// Owns the encoded bytes borrowed by libretro-common and one streaming decoder.
class studio_video_decoder
{
public:
    studio_video_decoder();
    ~studio_video_decoder();
    studio_video_decoder(const studio_video_decoder &) = delete;
    studio_video_decoder &operator=(const studio_video_decoder &) = delete;
    void open(const std::filesystem::path &);
    void close();
    void restart();
    bool update(double seconds, studio_video_frame &, double fallback_fps = 30);
private:
    struct impl;
    std::unique_ptr<impl> state;
};
