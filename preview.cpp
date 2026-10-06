#include "preview.h"
#include "shader_gl.h"
#include "glad.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <fstream>
#include <algorithm>
#include <vector>
#include <stdexcept>

studio_preview::~studio_preview()
{
    destroy();
}

void studio_preview::destroy()
{
    video.reset();
    if (input_fbo)
        glDeleteFramebuffers(1, &input_fbo);
    if (input_texture)
        glDeleteTextures(1, &input_texture);

    input_fbo = 0;
    input_texture = 0;
    output_texture = 0;
}

void studio_preview::abandon_context()
{
    video.reset();
    input_fbo = 0;
    input_texture = 0;
    output_texture = 0;
}

void studio_preview::upload(const unsigned char *data, unsigned w, unsigned h)
{
    GLint max_size = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_size);
    if (!w || !h || w > static_cast<unsigned>(max_size) || h > static_cast<unsigned>(max_size))
        throw std::runtime_error("Preview image exceeds this GPU's texture size limit.");

    GLint old_texture = 0;
    GLint old_draw_fbo = 0;
    GLint old_unpack_alignment = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw_fbo);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &old_unpack_alignment);

    if (input_texture && width == w && height == h)
    {
        glBindTexture(GL_TEXTURE_2D, input_texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, data);
        glPixelStorei(GL_UNPACK_ALIGNMENT, old_unpack_alignment);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(old_texture));
        return;
    }

    GLuint texture = 0;
    GLuint fbo = 0;

    glGenTextures(1, &texture);
    if (!texture)
        throw std::runtime_error("Cannot allocate preview texture.");

    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(w), static_cast<GLsizei>(h),
                 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

    glGenFramebuffers(1, &fbo);
    if (!fbo)
    {
        glPixelStorei(GL_UNPACK_ALIGNMENT, old_unpack_alignment);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(old_texture));
        glDeleteTextures(1, &texture);
        throw std::runtime_error("Cannot allocate preview framebuffer.");
    }

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

    if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(old_draw_fbo));
        glPixelStorei(GL_UNPACK_ALIGNMENT, old_unpack_alignment);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(old_texture));
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &texture);
        throw std::runtime_error("Cannot allocate preview framebuffer.");
    }

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(old_draw_fbo));
    glPixelStorei(GL_UNPACK_ALIGNMENT, old_unpack_alignment);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(old_texture));

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
    const unsigned char bars[8][3] = {
        {230, 230, 230}, {230, 230, 30}, {30, 230, 230}, {30, 230, 30},
        {230, 30, 230}, {230, 30, 30}, {30, 30, 230}, {25, 25, 25}};

    for (unsigned y = 0; y < 480; ++y)
        for (unsigned x = 0; x < 640; ++x)
        {
            auto *p = &pixels[(y * 640 + x) * 4];
            for (unsigned c = 0; c < 3; ++c)
                p[c] = y < 280 ? bars[x / 80][c]
                               : y < 380 ? static_cast<unsigned char>(x * 255 / 639)
                                         : ((x / 8 + y / 8) % 2 ? 230 : 25);
            p[3] = 255;
        }

    upload(pixels.data(), 640, 480);
    video.reset();
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
        upload(pixels, static_cast<unsigned>(w), static_cast<unsigned>(h));
    }
    catch (...)
    {
        stbi_image_free(pixels);
        throw;
    }

    stbi_image_free(pixels);
    video.reset();
    error.clear();
    return true;
}
catch (const std::exception &e)
{
    error = e.what();
    return false;
}

bool studio_preview::load_video(const std::filesystem::path &path, std::string &error)
try
{
    auto candidate = std::make_unique<studio_video_decoder>();
    candidate->open(path);
    studio_video_frame frame;
    candidate->update(0, frame);
    upload(frame.pixels.data(), frame.w, frame.h);
    video = std::move(candidate);
    video_time = 0;
    error.clear();
    return true;
}
catch (const std::exception &e)
{
    error = e.what();
    return false;
}

void studio_preview::restart_video()
{
    if (video) video->restart();
    video_time = 0;
}

bool studio_preview::advance_video(double delta, std::string &error)
try
{
    if (!video) return true;
    video_time += std::max(0.0, delta);
    studio_video_frame frame;
    if (video->update(video_time, frame))
        upload(frame.pixels.data(), frame.w, frame.h);
    return true;
}
catch (const std::exception &e)
{
    error = e.what();
    video.reset();
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

    GLint old_draw_fbo = 0;
    GLint texture = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    glGetFramebufferAttachmentParameteriv(
        GL_DRAW_FRAMEBUFFER,
        GL_COLOR_ATTACHMENT0,
        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,
        &texture);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(old_draw_fbo));

    output_texture = static_cast<unsigned>(texture);
    return fbo;
}
