#pragma once
#include <filesystem>
#include <string>
#include "video_decoder.h"

class studio_preview
{
public:
    unsigned width = 640, height = 480;
    unsigned input_texture = 0, input_fbo = 0, output_texture = 0;

    ~studio_preview(); // Destroy while the GL context is current.

    void destroy();         // Release preview-owned GL objects with a current context.
    void abandon_context(); // Forget invalid GL names after an externally destroyed context.

    void test_card();
    bool load_image(const std::filesystem::path &, std::string &error);
    bool load_video(const std::filesystem::path &, std::string &error);
    bool advance_video(double delta, std::string &error);
    void restart_video();
    unsigned render(unsigned output_width, unsigned output_height);

private:
    std::unique_ptr<studio_video_decoder> video;
    double video_time = 0;
    void upload(const unsigned char *, unsigned, unsigned);
};
