#include "preview.h"
#include "shader_gl.h"
#include "glad.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <fstream>
#include <vector>
#include <stdexcept>

studio_preview::~studio_preview()
{
    if (input_fbo)
        glDeleteFramebuffers(1, &input_fbo);
    if (input_texture)
        glDeleteTextures(1, &input_texture);
}
void studio_preview::upload(const unsigned char *data, unsigned w, unsigned h)
{
    GLint max_size = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_size);
    if (!w || !h || w > static_cast<unsigned>(max_size) || h > static_cast<unsigned>(max_size))
        throw std::runtime_error("Preview image exceeds this GPU's texture size limit.");
    GLuint texture = 0, fbo = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &texture);
    glTextureStorage2D(texture, 1, GL_RGBA8, w, h);
    glTextureParameteri(texture, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(texture, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureSubImage2D(texture, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glCreateFramebuffers(1, &fbo);
    glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0, texture, 0);
    if (glCheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &texture);
        throw std::runtime_error("Cannot allocate preview framebuffer.");
    }
    if (input_fbo)
        glDeleteFramebuffers(1, &input_fbo);
    if (input_texture)
        glDeleteTextures(1, &input_texture);
    input_texture = texture;
    input_fbo = fbo;
    output_texture = 0;
    width = w;
    height = h;
}
void studio_preview::test_card()
{
    std::vector<unsigned char> pixels(640 * 480 * 4);
    const unsigned char bars[8][3] = {{230, 230, 230}, {230, 230, 30}, {30, 230, 230}, {30, 230, 30}, 
    {230, 30, 230}, {230, 30, 30}, {30, 30, 230}, {25, 25, 25}};
    for (unsigned y = 0; y < 480; ++y)
        for (unsigned x = 0; x < 640; ++x)
        {
            auto *p = &pixels[(y * 640 + x) * 4];
            for (unsigned c = 0; c < 3; ++c)
                p[c] = y < 280 ? bars[x / 80][c] : y < 380 ? static_cast<unsigned char>(x * 255 / 639)
                                                           : ((x / 8 + y / 8) % 2 ? 230 : 25);
            p[3] = 255;
        }
    upload(pixels.data(), 640, 480);
}
bool studio_preview::load_image(const std::filesystem::path &path, std::string &error)
try
{
    auto size = std::filesystem::file_size(path);
    if (size > 64 * 1024 * 1024)
        throw std::runtime_error("Preview images must be smaller than 64 MiB.");
    std::ifstream in(path, std::ios::binary);
    std::vector<unsigned char> bytes(static_cast<size_t>(size));
    if (!in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size)))
        throw std::runtime_error("Cannot read preview image.");
    int w, h, channels;
    if (!stbi_info_from_memory(bytes.data(), static_cast<int>(size), &w, &h, &channels) || w > 8192 || h > 8192)
        throw std::runtime_error("Unsupported image or image larger than 8192 pixels.");
    auto *pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(size), &w, &h, &channels, 4);
    if (!pixels)
        throw std::runtime_error(stbi_failure_reason());
    try
    {
        upload(pixels, w, h);
    }
    catch (...)
    {
        stbi_image_free(pixels);
        throw;
    }
    stbi_image_free(pixels);
    error.clear();
    return true;
}
catch (const std::exception &e)
{
    error = e.what();
    return false;
}

unsigned studio_preview::render(unsigned w, unsigned h)
{
    auto fbo = shader_gl_render(input_fbo, width, height, w, h);
    if (!fbo)
    {
        output_texture = input_texture;
        return input_fbo;
    }
    GLint texture = 0;
    glGetNamedFramebufferAttachmentParameteriv(fbo, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &texture);
    output_texture = static_cast<unsigned>(texture);
    return fbo;
}
