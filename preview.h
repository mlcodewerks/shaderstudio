#pragma once
#include <filesystem>
#include <string>

class studio_preview
{
public:
    unsigned width = 640, height = 480;
    unsigned input_texture = 0, input_fbo = 0, output_texture = 0;
    ~studio_preview(); // Destroy while the GL context is current.
    void test_card();
    bool load_image(const std::filesystem::path &, std::string &error);
    unsigned render(unsigned output_width, unsigned output_height);

private:
    void upload(const unsigned char *, unsigned, unsigned);
};
