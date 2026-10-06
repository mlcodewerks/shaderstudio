#include "shadertoy_runtime.h"
#include "video_decoder.h"
#include "audio_decoder.h"
#include "glad.h"
#include "stb_image.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <complex>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <cstring>

namespace
{
    constexpr float pi = 3.14159265358979323846f;
    constexpr int sample_rate = 44100;
    bool gles() { return std::strstr(reinterpret_cast<const char *>(glGetString(GL_VERSION)), "OpenGL ES") != nullptr; }
    std::string glsl_header()
    {
        return gles() ? "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler3D;\n"
                      : "#version 330 core\n";
    }
    void check(bool ok, const std::string &s)
    {
        if (!ok)
            throw std::runtime_error(s);
    }
    std::vector<unsigned char> read_file(const std::filesystem::path &p, size_t limit)
    {
        auto size = std::filesystem::file_size(p);
        check(size <= limit, "Asset is too large: " + p.u8string());
        std::vector<unsigned char> data(static_cast<size_t>(size));
        std::ifstream f(p, std::ios::binary);
        check(static_cast<bool>(f.read(reinterpret_cast<char *>(data.data()), data.size())), "Cannot read " + p.u8string());
        return data;
    }
    std::string read_source(const std::filesystem::path &p)
    {
        auto data = read_file(p, 4 * 1024 * 1024);
        return shader_normalize_source(std::string(data.begin(), data.end()));
    }
    using image_data = studio_video_frame;
    image_data load_image(const std::filesystem::path &p)
    {
        auto bytes = read_file(p, 64 * 1024 * 1024);
        image_data out;
        int channels = 0;
        check(stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &out.w, &out.h, &channels) &&
                  out.w > 0 && out.h > 0 && out.w <= 8192 && out.h <= 8192,
              "Unsupported or oversized image: " + p.u8string());
        auto *pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &out.w, &out.h, &channels, 4);
        check(pixels != nullptr, "Cannot decode " + p.u8string());
        try
        {
            out.pixels.assign(pixels, pixels + static_cast<size_t>(out.w) * out.h * 4);
        }
        catch (...)
        {
            stbi_image_free(pixels);
            throw;
        }
        stbi_image_free(pixels);
        return out;
    }
    int levels(int w, int h, int d = 1) { return 1 + static_cast<int>(std::log2(std::max({w, h, d}))); }
    struct target
    {
        GLuint tex = 0, fbo = 0;
        int w = 0, h = 0, d = 1;
        GLenum type = GL_TEXTURE_2D, format = GL_RGBA32F;
        target() = default;
        target(const target &) = delete;
        target &operator=(const target &) = delete;
        ~target() { clear(); }
        void clear()
        {
            if (fbo)
                glDeleteFramebuffers(1, &fbo);
            if (tex)
                glDeleteTextures(1, &tex);
            tex = fbo = 0;
        }
        void bind() const { glBindTexture(type, tex); }
        void mipmaps() const { bind(); glGenerateMipmap(type); }
        void upload(int width, int height, GLenum channels, GLenum data_type, const void *pixels, int slice = 0)
        {
            bind();
            if (type == GL_TEXTURE_3D)
                glTexSubImage3D(type, 0, 0, 0, slice, width, height, 1, channels, data_type, pixels);
            else
                glTexSubImage2D(type == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + slice : type,
                                0, 0, 0, width, height, channels, data_type, pixels);
        }
        void zero()
        {
            // Framebuffer clears work on both desktop GL and GLES, including float targets.
            GLint previous;
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous);
            const GLfloat zeros[4]{};
            for (int face = 0; face < (type == GL_TEXTURE_CUBE_MAP ? 6 : 1); ++face)
            {
                attach(type == GL_TEXTURE_CUBE_MAP ? face : -1);
                glClearBufferfv(GL_COLOR, 0, zeros);
            }
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previous);
            mipmaps();
        }
        void create(int width, int height, GLenum texture_type = GL_TEXTURE_2D, GLenum texture_format = GL_RGBA32F, int depth = 1)
        {
            clear();
            w = width;
            h = height;
            d = depth;
            type = texture_type;
            // Half floats are filterable in GLES 3 without OES_texture_float_linear.
            format = gles() && texture_format == GL_RGBA32F ? GL_RGBA16F : texture_format;
            glGenTextures(1, &tex);
            bind();
            GLenum channels = format == GL_R8 ? GL_RED : GL_RGBA;
            GLenum data_type = format == GL_RGBA32F || format == GL_RGBA16F ? GL_FLOAT : GL_UNSIGNED_BYTE;
            for (int level = 0; level < levels(w, h, d); ++level)
            {
                int lw = std::max(1, w >> level), lh = std::max(1, h >> level);
                if (type == GL_TEXTURE_3D)
                    glTexImage3D(type, level, format, lw, lh, std::max(1, d >> level), 0, channels, data_type, nullptr);
                else
                    for (int face = 0; face < (type == GL_TEXTURE_CUBE_MAP ? 6 : 1); ++face)
                        glTexImage2D(type == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : type,
                                     level, format, lw, lh, 0, channels, data_type, nullptr);
            }
            glTexParameteri(type, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(type, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(type, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(type, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(type, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
            // Volume assets are uploaded completely before use.
            if (type != GL_TEXTURE_3D)
                zero();
        }
        void attach(int face = -1)
        {
            if (!fbo)
                glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                face < 0 ? GL_TEXTURE_2D : GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, tex, 0);
            check(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
                "Cannot allocate Shadertoy framebuffer (GLES requires floating-point color buffer support).");
        }
    };
    struct gl_scope
    {
        GLint draw, read, program, vao, viewport[4];
        GLboolean blend, depth, scissor, cull, srgb, stencil, discard, mask[4];
        GLint active, pack, unpack, pack_alignment, unpack_alignment;
        GLint pack_row, pack_skip_rows, pack_skip_pixels, unpack_row, unpack_skip_rows, unpack_skip_pixels, unpack_height, unpack_skip_images;
        bool es = gles();
        gl_scope()
        {
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
            glGetIntegerv(GL_CURRENT_PROGRAM, &program);
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
            glGetIntegerv(GL_VIEWPORT, viewport);
            blend = glIsEnabled(GL_BLEND);
            depth = glIsEnabled(GL_DEPTH_TEST);
            scissor = glIsEnabled(GL_SCISSOR_TEST);
            cull = glIsEnabled(GL_CULL_FACE);
            srgb = !es && glIsEnabled(GL_FRAMEBUFFER_SRGB);
            stencil = glIsEnabled(GL_STENCIL_TEST);
            discard = glIsEnabled(GL_RASTERIZER_DISCARD);
            glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
            glActiveTexture(GL_TEXTURE0);
            glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
            glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
#define PIXEL_STATE(key, value, clean) glGetIntegerv(key, &value); glPixelStorei(key, clean)
            PIXEL_STATE(GL_PACK_ALIGNMENT, pack_alignment, 1);
            PIXEL_STATE(GL_UNPACK_ALIGNMENT, unpack_alignment, 1);
            PIXEL_STATE(GL_PACK_ROW_LENGTH, pack_row, 0);
            PIXEL_STATE(GL_PACK_SKIP_ROWS, pack_skip_rows, 0);
            PIXEL_STATE(GL_PACK_SKIP_PIXELS, pack_skip_pixels, 0);
            PIXEL_STATE(GL_UNPACK_ROW_LENGTH, unpack_row, 0);
            PIXEL_STATE(GL_UNPACK_SKIP_ROWS, unpack_skip_rows, 0);
            PIXEL_STATE(GL_UNPACK_SKIP_PIXELS, unpack_skip_pixels, 0);
            PIXEL_STATE(GL_UNPACK_IMAGE_HEIGHT, unpack_height, 0);
            PIXEL_STATE(GL_UNPACK_SKIP_IMAGES, unpack_skip_images, 0);
#undef PIXEL_STATE
            glGetBooleanv(GL_COLOR_WRITEMASK, mask);
            glDisable(GL_BLEND);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_CULL_FACE);
            if (!es) glDisable(GL_FRAMEBUFFER_SRGB);
            glDisable(GL_STENCIL_TEST);
            glDisable(GL_RASTERIZER_DISCARD);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        }
        ~gl_scope()
        {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
            glUseProgram(program);
            glBindVertexArray(vao);
            glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
            auto restore = [](GLenum e, bool b)
            { if (b) glEnable(e); else glDisable(e); };
            restore(GL_BLEND, blend);
            restore(GL_DEPTH_TEST, depth);
            restore(GL_SCISSOR_TEST, scissor);
            restore(GL_CULL_FACE, cull);
            if (!es) restore(GL_FRAMEBUFFER_SRGB, srgb);
            restore(GL_STENCIL_TEST, stencil);
            restore(GL_RASTERIZER_DISCARD, discard);
            glColorMask(mask[0], mask[1], mask[2], mask[3]);
            for (int i = 0; i < 4; ++i)
            {
                glActiveTexture(GL_TEXTURE0 + i);
                glBindTexture(GL_TEXTURE_2D, 0);
                glBindTexture(GL_TEXTURE_3D, 0);
                glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
                glBindSampler(i, 0);
            }
            glActiveTexture(active);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, pack);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack);
            glPixelStorei(GL_PACK_ALIGNMENT, pack_alignment);
            glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_alignment);
            glPixelStorei(GL_PACK_ROW_LENGTH, pack_row);
            glPixelStorei(GL_PACK_SKIP_ROWS, pack_skip_rows);
            glPixelStorei(GL_PACK_SKIP_PIXELS, pack_skip_pixels);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, unpack_row);
            glPixelStorei(GL_UNPACK_SKIP_ROWS, unpack_skip_rows);
            glPixelStorei(GL_UNPACK_SKIP_PIXELS, unpack_skip_pixels);
            glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, unpack_height);
            glPixelStorei(GL_UNPACK_SKIP_IMAGES, unpack_skip_images);
        }
    };
    void uniform(GLuint p, const char *n, float v) { glUseProgram(p); glUniform1f(glGetUniformLocation(p, n), v); }
    void integer(GLuint p, const char *n, int v) { glUseProgram(p); glUniform1i(glGetUniformLocation(p, n), v); }
    GLuint shader(GLenum type, const std::string &text, std::string &log)
    {
        GLuint s = glCreateShader(type);
        const char *str = text.c_str();
        glShaderSource(s, 1, &str, nullptr);
        glCompileShader(s);
        GLint ok = 0, size = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        glGetShaderiv(s, GL_INFO_LOG_LENGTH, &size);
        if (size > 1)
        {
            std::string msg(size, '\0');
            glGetShaderInfoLog(s, size, nullptr, msg.data());
            log += msg.c_str();
        }
        if (!ok)
        {
            glDeleteShader(s);
            return 0;
        }
        return s;
    }
    std::string header(const toy_pass &p)
    {
        std::string s = glsl_header() + R"(
#define texture2D texture
#define textureCube texture
#define iGlobalTime iTime
uniform vec3 iResolution;
uniform float iTime, iTimeDelta, iFrameRate, iSampleRate;
uniform int iFrame;
uniform float iChannelTime[4];
uniform vec3 iChannelResolution[4];
uniform vec4 iMouse, iDate;
uniform int studioFace, studioSampleOffset;
layout(location=0) out vec4 studioColor;
)";
        for (int i = 0; i < 4; ++i)
        {
            auto k = p.channels[i].kind;
            s += "uniform " + std::string(k == toy_input_kind::cubemap || k == toy_input_kind::cube_texture ? 
                "samplerCube" : k == toy_input_kind::volume ? "sampler3D" : "sampler2D") +
                 " iChannel" + std::to_string(i) + ";\n";
        }
        return s;
    }
    std::string footer(int slot, bool vr)
    {
        if (slot == 6)
            return R"(
#line 1 102
void main() { int s = studioSampleOffset + int(gl_FragCoord.x) + int(gl_FragCoord.y)*1024;
    studioColor = vec4(mainSound(s, float(s)/iSampleRate), 0, 1); }
)";
        if (slot == 4)
            return R"(
#line 1 102
void main() {
    vec2 p = 2.0 * gl_FragCoord.xy / iResolution.xy - 1.0;
    vec3 d = studioFace == 0 ? vec3(1,-p.y,-p.x) : studioFace == 1 ? vec3(-1,-p.y,p.x) :
             studioFace == 2 ? vec3(p.x,1,p.y) : studioFace == 3 ? vec3(p.x,-1,-p.y) :
             studioFace == 4 ? vec3(p.x,-p.y,1) : vec3(-p.x,-p.y,-1);
    mainCubemap(studioColor, gl_FragCoord.xy, vec3(0), normalize(d));
}
)";
        if (slot == 5 && vr)
            return R"(
#line 1 102
void main() { vec2 p = (2.0 * gl_FragCoord.xy-iResolution.xy)/iResolution.y;
    mainVR(studioColor, gl_FragCoord.xy, vec3(0), normalize(vec3(p,-1))); }
)";
        return "\n#line 1 102\nvoid main() { mainImage(studioColor, gl_FragCoord.xy); }\n";
    }
    void parse_log(shader_diagnostic_report &report, const std::string &log,
                   const std::filesystem::path &common, const std::filesystem::path &pass)
    {
        // NVIDIA: 101(7), Mesa: 101:7(...), AMD: ERROR: 101:7: ...
        static const std::regex location(R"((\d+)[(:](\d+))");
        std::istringstream in(log);
        std::string line;
        while (std::getline(in, line))
        {
            std::smatch m;
            if (std::regex_search(line, m, location))
            {
                int id = std::stoi(m[1]);
                auto path = id == 100 ? common : id == 101 ? pass
                                                           : std::filesystem::path{};
                if (!path.empty())
                    report.entries.push_back({std::filesystem::weakly_canonical(path), static_cast<size_t>(std::stoul(m[2])),
                                              line.find("warning") != line.npos, line});
            }
        }
    }
    // Web Audio's default analyser: 2048-point Blackman-window FFT, smoothed dB bins.
    void audio_texture(target &out, const float *samples, std::array<float, 512> &smooth)
    {
        std::array<std::complex<float>, 2048> fft;
        for (int i = 0; i < 2048; ++i)
        {
            float a = 2 * pi * i / 2048;
            fft[i] = samples[i] * (0.42f - 0.5f * std::cos(a) + 0.08f * std::cos(2 * a));
        }
        for (unsigned i = 1, j = 0; i < 2048; ++i)
        {
            unsigned bit = 1024;
            for (; j & bit; bit >>= 1)
                j ^= bit;
            j ^= bit;
            if (i < j)
                std::swap(fft[i], fft[j]);
        }
        for (int len = 2; len <= 2048; len *= 2)
        {
            auto step = std::polar(1.f, -2 * pi / len);
            for (int i = 0; i < 2048; i += len)
            {
                std::complex<float> w(1, 0);
                for (int j = 0; j < len / 2; ++j)
                {
                    auto u = fft[i + j], v = fft[i + j + len / 2] * w;
                    fft[i + j] = u + v;
                    fft[i + j + len / 2] = u - v;
                    w *= step;
                }
            }
        }
        std::array<unsigned char, 1024> pixels{};
        for (int i = 0; i < 512; ++i)
        {
            smooth[i] = 0.8f * smooth[i] + 0.2f * std::abs(fft[i]) / 2048;
            float db = 20.f * std::log10(std::max(1e-10f, smooth[i]));
            pixels[i] = static_cast<unsigned char>(255 * std::clamp((db + 100) / 70, 0.f, 1.f));
            pixels[i + 512] = static_cast<unsigned char>(255 * std::clamp(0.5f + samples[i + 1536] * 0.5f, 0.f, 1.f));
        }
        out.upload(512, 2, GL_RED, GL_UNSIGNED_BYTE, pixels.data());
        out.mipmaps();
    }
    struct channel_asset
    {
        target image;
        std::vector<float> audio;
        std::vector<float> stereo;
        SDL_AudioStream *playback = nullptr;
        int64_t audio_position = 0;
        std::vector<std::filesystem::path> sequence;
        studio_video_decoder video;
        int sequence_frame = -1;
        std::array<float, 512> smooth{};
        SDL_AudioStream *mic = nullptr;
        SDL_Camera *camera = nullptr;
        bool capture_failed = false;
        std::array<float, 2048> mic_samples{};
        ~channel_asset()
        {
            stop();
            if (playback)
                SDL_DestroyAudioStream(playback);
        }
        void stop()
        {
            if (mic)
                SDL_DestroyAudioStream(mic);
            if (camera)
                SDL_CloseCamera(camera);
            mic = nullptr;
            camera = nullptr;
        }
        void upload(const image_data &data, const toy_channel &c)
        {
            int slices = c.kind == toy_input_kind::cube_texture ? 6 : c.kind == toy_input_kind::volume ? c.depth
                                                                                                       : 1;
            check(data.w % slices == 0, "Atlas width must be divisible by its slice count.");
            int w = data.w / slices, h = data.h;
            if (c.kind == toy_input_kind::cube_texture)
                check(w == h, "Cubemap requires six square faces in +X,-X,+Y,-Y,+Z,-Z order.");
            GLenum type = c.kind == toy_input_kind::cube_texture ? GL_TEXTURE_CUBE_MAP : c.kind == toy_input_kind::volume ? GL_TEXTURE_3D
                                                                                                                          : GL_TEXTURE_2D;
            GLint max_size = 0;
            glGetIntegerv(type == GL_TEXTURE_3D ? GL_MAX_3D_TEXTURE_SIZE : type == GL_TEXTURE_CUBE_MAP ? GL_MAX_CUBE_MAP_TEXTURE_SIZE
                                                                                                       : GL_MAX_TEXTURE_SIZE,
                          &max_size);
            check(w <= max_size && h <= max_size && (type != GL_TEXTURE_3D || slices <= max_size), "Channel exceeds GPU texture limits.");
            if (!image.tex || image.w != w || image.h != h)
                image.create(w, h, type, c.srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, type == GL_TEXTURE_3D ? slices : 1);
            std::vector<unsigned char> pixels(static_cast<size_t>(w) * h * 4);
            for (int s = 0; s < slices; ++s)
            {
                for (int y = 0; y < h; ++y)
                    std::copy_n(data.pixels.data() + (static_cast<size_t>(c.vflip ? h - 1 - y : y) * data.w + s * w) * 4,
                                w * 4, pixels.data() + static_cast<size_t>(y) * w * 4);
                image.upload(w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data(), s);
            }
            image.mipmaps();
        }
        void load(const toy_channel &c)
        {
            if (c.kind == toy_input_kind::texture || c.kind == toy_input_kind::cube_texture || c.kind == toy_input_kind::volume)
                upload(load_image(c.file), c);
            if (c.kind == toy_input_kind::video)
            {
                if (!std::filesystem::is_directory(c.file))
                {
                    image_data data;
                    video.open(c.file);
                    video.update(0, data, c.fps);
                    upload(data, c);
                }
                else
                {
                    for (const auto &e : std::filesystem::directory_iterator(c.file))
                    {
                        auto ext = e.path().extension().u8string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char x)
                                       { return std::tolower(x); });
                        if (e.is_regular_file() && (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga"))
                            sequence.push_back(e.path());
                        check(sequence.size() <= 10000, "Video sequence exceeds 10000 frames.");
                    }
                    std::sort(sequence.begin(), sequence.end());
                    check(!sequence.empty(), "Video sequence is empty.");
                    upload(load_image(sequence[0]), c);
                    sequence_frame = 0;
                }
            }
            if (c.kind == toy_input_kind::audio || c.kind == toy_input_kind::microphone)
                image.create(512, 2, GL_TEXTURE_2D, GL_R8);
            if (c.kind == toy_input_kind::camera)
                image.create(1, 1, GL_TEXTURE_2D, GL_RGBA8);
            if (c.kind == toy_input_kind::audio)
            {
                stereo = studio_decode_audio(c.file, sample_rate);
                audio.resize(stereo.size() / 2);
                for (size_t i = 0; i < audio.size(); ++i)
                    audio[i] = (stereo[2 * i] + stereo[2 * i + 1]) * 0.5f;
            }
        }
        float update(const toy_channel &c, float time, bool capture, bool listen, std::string &status)
        {
            if (!capture)
            {
                stop();
                capture_failed = false;
            }
            if (capture_failed)
                return 0;
            if (c.kind == toy_input_kind::video)
            {
                if (sequence.empty())
                {
                    image_data data;
                    if (video.update(time, data, c.fps))
                        upload(data, c);
                    return time;
                }
                int index = static_cast<int>(std::fmod(std::floor(time * c.fps), static_cast<double>(sequence.size())));
                if (index != sequence_frame)
                {
                    upload(load_image(sequence[index]), c);
                    sequence_frame = index;
                }
                return std::fmod(time, sequence.size() / c.fps);
            }
            if (c.kind == toy_input_kind::audio)
            {
                std::array<float, 2048> samples{};
                int64_t pos = static_cast<int64_t>(time * sample_rate);
                for (int i = 0; i < 2048; ++i)
                {
                    auto at = pos - 2048 + i;
                    if (at >= 0)
                        samples[i] = audio[at % audio.size()];
                }
                audio_texture(image, samples.data(), smooth);
                if (listen)
                {
                    if (!playback)
                    {
                        check(SDL_WasInit(SDL_INIT_AUDIO) || SDL_InitSubSystem(SDL_INIT_AUDIO), SDL_GetError());
                        SDL_AudioSpec spec{SDL_AUDIO_F32, 2, sample_rate};
                        playback = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
                        check(playback != nullptr, SDL_GetError());
                        SDL_ResumeAudioStreamDevice(playback);
                    }
                    audio_position = std::max(audio_position, pos);
                    int queued = SDL_GetAudioStreamQueued(playback);
                    for (int b = 0; b < 8 && audio_position < pos + sample_rate / 12 && queued < sample_rate * 2 * 4 / 4; ++b)
                    {
                        std::array<float, 2048> block{};
                        for (int i = 0; i < 1024; ++i)
                        {
                            size_t at = static_cast<size_t>((audio_position + i) % audio.size());
                            block[2 * i] = stereo[2 * at];
                            block[2 * i + 1] = stereo[2 * at + 1];
                        }
                        SDL_PutAudioStreamData(playback, block.data(), sizeof(block));
                        audio_position += 1024;
                        queued += sizeof(block);
                    }
                }
                return std::fmod(time, static_cast<float>(audio.size()) / sample_rate);
            }
            if (capture && c.kind == toy_input_kind::microphone)
            {
                if (!mic)
                {
                    check(SDL_WasInit(SDL_INIT_AUDIO) || SDL_InitSubSystem(SDL_INIT_AUDIO), SDL_GetError());
                    SDL_AudioSpec spec{SDL_AUDIO_F32, 1, sample_rate};
                    mic = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, nullptr, nullptr);
                    check(mic != nullptr, SDL_GetError());
                    check(SDL_ResumeAudioStreamDevice(mic), SDL_GetError());
                }
                int count = SDL_GetAudioStreamAvailable(mic) / sizeof(float);
                if (count > 0)
                {
                    std::vector<float> samples(std::min(count, sample_rate));
                    int got = SDL_GetAudioStreamData(mic, samples.data(), samples.size() * sizeof(float));
                    check(got >= 0, SDL_GetError());
                    count = got / sizeof(float);
                    int take = std::min(count, 2048);
                    std::move(mic_samples.begin() + take, mic_samples.end(), mic_samples.begin());
                    std::copy_n(samples.data() + count - take, take, mic_samples.end() - take);
                }
                audio_texture(image, mic_samples.data(), smooth);
                return time;
            }
            if (capture && c.kind == toy_input_kind::camera)
            {
                if (!camera)
                {
                    check(SDL_WasInit(SDL_INIT_CAMERA) || SDL_InitSubSystem(SDL_INIT_CAMERA), SDL_GetError());
                    int count = 0;
                    auto *ids = SDL_GetCameras(&count);
                    if (count > 0)
                        camera = SDL_OpenCamera(ids[0], nullptr);
                    SDL_free(ids);
                    check(camera != nullptr, SDL_GetError());
                }
                auto permission = SDL_GetCameraPermissionState(camera);
                // SDL 3.2 returns an int; SDL 3.4 names the same -1/0/1 states.
                check(permission >= 0, "Camera access was denied.");
                Uint64 timestamp = 0;
                auto *surface = SDL_AcquireCameraFrame(camera, &timestamp);
                if (surface)
                {
                    auto *rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
                    SDL_ReleaseCameraFrame(camera, surface);
                    check(rgba != nullptr, SDL_GetError());
                    image_data data;
                    data.w = rgba->w;
                    data.h = rgba->h;
                    try
                    {
                        data.pixels.resize(static_cast<size_t>(data.w) * data.h * 4);
                        for (int y = 0; y < data.h; ++y)
                            std::copy_n(static_cast<unsigned char *>(rgba->pixels) + y * rgba->pitch, data.w * 4, 
                            data.pixels.data() + static_cast<size_t>(y) * data.w * 4);
                    }
                    catch (...)
                    {
                        SDL_DestroySurface(rgba);
                        throw;
                    }
                    SDL_DestroySurface(rgba);
                    upload(data, c);
                }
                else
                    status = "Waiting for camera frame or camera permission.";
                return time;
            }
            return 0;
        }
    };
}

struct shadertoy_runtime::state
{
    shadertoy_project project;
    std::array<GLuint, 7> programs{};
    std::array<std::array<target, 2>, 6> buffers;
    std::array<int, 6> front{};
    std::array<std::array<channel_asset, 4>, 7> assets;
    std::array<std::array<GLuint, 4>, 7> samplers{};
    target keyboard, black, sound;
    GLuint vao = 0;
    unsigned width = 0, height = 0;
    int frame = 0, sound_sample = 0;
    double time = 0;
    SDL_AudioStream *output = nullptr;
    bool output_failed = false;
    ~state()
    {
        for (auto p : programs)
            if (p)
                glDeleteProgram(p);
        for (auto &s : samplers)
            if (s[0]) glDeleteSamplers(4, s.data());
        if (vao)
            glDeleteVertexArrays(1, &vao);
        if (output)
            SDL_DestroyAudioStream(output);
    }
};
shadertoy_runtime::shadertoy_runtime() = default;
shadertoy_runtime::~shadertoy_runtime() = default;
void shadertoy_runtime::abandon_context()
{
    if (current)
    {
        current->programs.fill(0);
        for (auto &samplers : current->samplers) samplers.fill(0);
        current->vao = 0;
        auto forget = [](target &t) { t.tex = t.fbo = 0; };
        for (auto &pair : current->buffers) for (auto &t : pair) forget(t);
        for (auto &pass : current->assets) for (auto &asset : pass) forget(asset.image);
        forget(current->keyboard);
        forget(current->black);
        forget(current->sound);
    }
    clear();
}
void shadertoy_runtime::clear()
{
    current.reset();
    texture = 0;
    error.clear();
    diagnostics.clear();
    keys.fill(0);
    pointer.fill(0);
}
float shadertoy_runtime::time() const { return current ? static_cast<float>(current->time) : 0; }
int shadertoy_runtime::frame() const { return current ? current->frame : 0; }
void shadertoy_runtime::reset()
{
    if (!current)
        return;
    auto &s = *current;
    s.time = 0;
    s.frame = s.sound_sample = 0;
    s.front.fill(0);
    keys.fill(0);
    pointer.fill(0);
    gl_scope scope;
    for (auto &pair : s.buffers)
        for (auto &b : pair)
            if (b.tex)
            {
                b.zero();
            }
    for (auto &pass : s.assets)
        for (auto &a : pass)
        {
            a.smooth.fill(0);
            a.mic_samples.fill(0);
            a.audio_position = 0;
            if (a.playback)
                SDL_ClearAudioStream(a.playback);
        }
    if (s.output)
        SDL_ClearAudioStream(s.output);
}
void shadertoy_runtime::key(unsigned code, bool down, bool repeat)
{
    if (code >= 256)
        return;
    if (down && !repeat && !keys[code])
    {
        keys[256 + code] = 255;
        keys[512 + code] ^= 255;
    }
    keys[code] = down ? 255 : 0;
}
void shadertoy_runtime::release_keys() { std::fill_n(keys.begin(), 512, 0); }
void shadertoy_runtime::mouse(float x, float y, bool down, bool clicked)
{
    if (down)
    {
        pointer[0] = x;
        pointer[1] = y;
    }
    if (clicked)
    {
        pointer[2] = std::max(x, 0.0001f);
        pointer[3] = std::max(y, 0.0001f);
    }
    if (!down)
        pointer[2] = -std::abs(pointer[2]);
}
void shadertoy_runtime::set_playing(bool value)
{
    playing = value;
    if (current && (!value || !capture_enabled))
        for (auto &pass : current->assets)
            for (auto &asset : pass)
            {
                asset.stop();
                asset.capture_failed = false;
            }
    if (current && !sound_enabled)
        current->output_failed = false;
    if (current)
        for (auto &pass : current->assets)
            for (auto &asset : pass)
                if (asset.playback)
                {
                    if (value && sound_enabled)
                        SDL_ResumeAudioStreamDevice(asset.playback);
                    else
                    {
                        SDL_PauseAudioStreamDevice(asset.playback);
                        SDL_ClearAudioStream(asset.playback);
                        asset.audio_position = static_cast<int64_t>(current->time * sample_rate);
                    }
                }
    if (current && current->output)
    {
        if (value && sound_enabled)
            SDL_ResumeAudioStreamDevice(current->output);
        else
        {
            SDL_PauseAudioStreamDevice(current->output);
            SDL_ClearAudioStream(current->output);
            current->sound_sample = static_cast<int>(current->time * sample_rate);
        }
    }
}
bool shadertoy_runtime::compile(const shadertoy_project &project)
try
{
    diagnostics.clear();
    error.clear();
    media_status.clear();
    if (!project.validate(error))
        return false;
    gl_scope scope;
    auto next = std::make_unique<state>();
    next->project = project;
    std::string common;
    if (!project.common.empty())
    {
        common = read_source(project.common);
        diagnostics.sources[std::filesystem::weakly_canonical(project.common)] = common;
    }
    for (int i = 0; i < 7; ++i)
    {
        const auto &p = project.passes[i];
        if (p.source.empty())
            continue;
        auto source = read_source(p.source);
        diagnostics.sources[std::filesystem::weakly_canonical(p.source)] = source;
        std::string log;
        GLuint fs = shader(GL_FRAGMENT_SHADER, header(p) + "\n#line 1 100\n" + common + "\n#line 1 101\n" + source + footer(i, project.vr), log);
        parse_log(diagnostics, log, project.common, p.source);
        check(fs != 0, std::string(toy_pass_name(static_cast<toy_pass_kind>(i))) + ":\n" + log);
        GLuint vs = shader(GL_VERTEX_SHADER, glsl_header() + \
            "void main(){ vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"\
            "gl_Position=vec4(p*2.-1.,0,1); }", log);
        if (!vs)
        {
            glDeleteShader(fs);
            throw std::runtime_error(log);
        }
        auto program = next->programs[i] = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);
        glDeleteShader(fs);
        glDeleteShader(vs);
        GLint ok = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok)
        {
            GLint len = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &len);
            std::string msg(std::max(1, len), '\0');
            glGetProgramInfoLog(program, len, nullptr, msg.data());
            throw std::runtime_error(msg);
        }
        glGenSamplers(4, next->samplers[i].data());
        for (int j = 0; j < 4; ++j)
        {
            const auto &c = p.channels[j];
            auto sampler = next->samplers[i][j];
            glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, c.filter == 0 ? GL_NEAREST : c.filter == 1 ? GL_LINEAR
                                                                                                           : GL_LINEAR_MIPMAP_LINEAR);
            glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, c.filter == 0 ? GL_NEAREST : GL_LINEAR);
            for (auto axis : {GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, GL_TEXTURE_WRAP_R})
                glSamplerParameteri(sampler, axis, c.wrap ? GL_REPEAT : GL_CLAMP_TO_EDGE);
            integer(program, ("iChannel" + std::to_string(j)).c_str(), j);
            next->assets[i][j].load(c);
        }
    }
    next->keyboard.create(256, 3, GL_TEXTURE_2D, GL_R8);
    next->black.create(1, 1, GL_TEXTURE_2D, GL_RGBA8);
    glGenVertexArrays(1, &next->vao);
    current = std::move(next);
    texture = 0;
    reset();
    error.clear();
    return true;
}
catch (const std::exception &e)
{
    error = e.what();
    return false;
}

unsigned shadertoy_runtime::render(unsigned width, unsigned height, float delta)
try
{
    if (!current)
        return 0;
    check(width > 0 && height > 0 && width <= 4096 && height <= 4096, "Preview size must be between 1 and 4096.");
    check(std::isfinite(delta) && delta >= 0, "Invalid preview time delta.");
    gl_scope scope;
    auto &s = *current;
    if (s.width != width || s.height != height)
    {
        // Allocate all new targets before replacing the working framebuffers.
        uint64_t bytes = 0;
        for (int i = 0; i < 6; ++i)
            if (s.programs[i])
                bytes += i == 4 ? static_cast<uint64_t>(std::min(width, height)) * std::min(width, height) * 6 * 16 * 2 * 4 / 3 : 
                static_cast<uint64_t>(width) * height * 16 * 2 * 4 / 3;
        check(bytes <= 512ull * 1024 * 1024, "Shadertoy framebuffers exceed 512 MiB; reduce preview dimensions or remove passes.");
        auto replacement = std::make_unique<std::array<std::array<target, 2>, 6>>();
        for (int i = 0; i < 6; ++i)
            if (s.programs[i])
                for (auto &b : (*replacement)[i])
                {
                    int w = i == 4 ? std::min(width, height) : width, h = i == 4 ? w : height;
                    b.create(w, h, i == 4 ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D);
                    b.attach(i == 4 ? 0 : -1);
                }
        for (int i = 0; i < 6; ++i)
            for (int j = 0; j < 2; ++j)
            {
                auto &a = s.buffers[i][j], &b = (*replacement)[i][j];
                std::swap(a.tex, b.tex);
                std::swap(a.fbo, b.fbo);
                std::swap(a.w, b.w);
                std::swap(a.h, b.h);
                std::swap(a.d, b.d);
                std::swap(a.type, b.type);
                std::swap(a.format, b.format);
            }
        s.width = width;
        s.height = height;
        reset();
    }
    glBindVertexArray(s.vao);
    s.keyboard.upload(256, 3, GL_RED, GL_UNSIGNED_BYTE, keys.data());
    s.keyboard.mipmaps();
    auto now = std::chrono::system_clock::now();
    auto stamp = std::chrono::system_clock::to_time_t(now);
    std::tm date = *std::localtime(&stamp);
    double epoch = std::chrono::duration<double>(now.time_since_epoch()).count();
    float seconds = static_cast<float>(date.tm_hour * 3600 + date.tm_min * 60 + date.tm_sec + epoch - std::floor(epoch));
    float dates[4] = {static_cast<float>(date.tm_year + 1900), static_cast<float>(date.tm_mon), static_cast<float>(date.tm_mday), seconds};
    auto bind = [&](int i, int w, int h)
    {
        GLuint p = s.programs[i];
        glUseProgram(p);
        glViewport(0, 0, w, h);
        glUniform3f(glGetUniformLocation(p, "iResolution"), static_cast<float>(w), static_cast<float>(h), 1);
        uniform(p, "iTime", static_cast<float>(s.time));
        uniform(p, "iTimeDelta", delta);
        uniform(p, "iFrameRate", delta > 0 ? 1 / delta : 0);
        integer(p, "iFrame", s.frame);
        uniform(p, "iSampleRate", sample_rate);
        glUniform4fv(glGetUniformLocation(p, "iMouse"), 1, pointer.data());
        glUniform4fv(glGetUniformLocation(p, "iDate"), 1, dates);
        float resolutions[12]{}, times[4]{};
        for (int j = 0; j < 4; ++j)
        {
            glActiveTexture(GL_TEXTURE0 + j);
            const auto &c = s.project.passes[i].channels[j];
            auto &asset = s.assets[i][j];
            target *input = &s.black;
            int kind = static_cast<int>(c.kind);
            if (kind >= 1 && kind <= 5)
            {
                input = &s.buffers[kind - 1][s.front[kind - 1]];
                times[j] = static_cast<float>(s.time);
            }
            else if (c.kind == toy_input_kind::keyboard)
                input = &s.keyboard;
            else if (asset.image.tex)
            {
                input = &asset.image;
                bool listen = sound_enabled && playing && !audio_output;
                // The same music track can feed several shader channels, but plays once.
                if (c.kind == toy_input_kind::audio)
                    for (int a = 0; a <= i; ++a)
                        for (int b = 0; b < (a == i ? j : 4); ++b)
                            if (!s.project.passes[a].source.empty() && s.project.passes[a].channels[b].kind == 
                            toy_input_kind::audio && s.project.passes[a].channels[b].file == c.file)
                                listen = false;
                try
                {
                    times[j] = asset.update(c, static_cast<float>(s.time), capture_enabled && playing, listen, media_status);
                }
                catch (const std::exception &e)
                {
                    media_status = e.what();
                    if (c.kind == toy_input_kind::microphone || c.kind == toy_input_kind::camera)
                    {
                        asset.stop();
                        asset.capture_failed = true;
                    }
                }
            }
            if (c.kind != toy_input_kind::none)
            {
                resolutions[j * 3] = static_cast<float>(input->w);
                resolutions[j * 3 + 1] = static_cast<float>(input->h);
                resolutions[j * 3 + 2] = static_cast<float>(input->d);
            }
            glActiveTexture(GL_TEXTURE0 + j);
            input->bind();
            glBindSampler(j, s.samplers[i][j]);
        }
        glUniform3fv(glGetUniformLocation(p, "iChannelResolution[0]"), 4, resolutions);
        glUniform1fv(glGetUniformLocation(p, "iChannelTime[0]"), 4, times);
    };
    for (int i = 0; i < 6; ++i)
        if (s.programs[i])
        {
            auto &dest = s.buffers[i][1 - s.front[i]];
            bind(i, dest.w, dest.h);
            for (int face = 0; face < (i == 4 ? 6 : 1); ++face)
            {
                dest.attach(i == 4 ? face : -1);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dest.fbo);
                integer(s.programs[i], "studioFace", face);
                glDrawArrays(GL_TRIANGLES, 0, 3);
            }
            dest.mipmaps();
            s.front[i] = 1 - s.front[i];
        }
    if (sound_enabled && playing && s.programs[6] && !audio_output)
    {
        if (!s.output && !s.output_failed)
        {
            if (SDL_WasInit(SDL_INIT_AUDIO) || SDL_InitSubSystem(SDL_INIT_AUDIO))
            {
                SDL_AudioSpec spec{SDL_AUDIO_F32, 2, sample_rate};
                s.output = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
            }
            if (!s.output)
            {
                media_status = SDL_GetError();
                s.output_failed = true;
            }
            else
                SDL_ResumeAudioStreamDevice(s.output);
        }
        if (s.output)
        {
            if (!s.sound.tex)
            {
                s.sound.create(1024, 1);
                s.sound.attach();
            }
            // Queue a short horizon; the GPU generates stereo float samples directly.
            int queued = SDL_GetAudioStreamQueued(s.output);
            int desired = static_cast<int>((s.time + 0.08) * sample_rate);
            s.sound_sample = std::max(s.sound_sample, static_cast<int>(s.time * sample_rate));
            for (int block = 0; block < 8 && s.sound_sample < desired && queued < sample_rate * 2 * 4 / 4; ++block)
            {
                bind(6, 1024, 1);
                integer(s.programs[6], "studioSampleOffset", s.sound_sample);
                glBindFramebuffer(GL_FRAMEBUFFER, s.sound.fbo);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                std::array<float, 4096> rgba{};
                std::array<float, 2048> samples{};
                glReadPixels(0, 0, 1024, 1, GL_RGBA, GL_FLOAT, rgba.data());
                for (int i = 0; i < 1024; ++i)
                {
                    samples[2 * i] = rgba[4 * i];
                    samples[2 * i + 1] = rgba[4 * i + 1];
                }
                for (auto &v : samples)
                    v = std::isfinite(v) ? std::clamp(v, -1.f, 1.f) : 0.f;
                SDL_PutAudioStreamData(s.output, samples.data(), sizeof(samples));
                s.sound_sample += 1024;
                queued += sizeof(samples);
            }
        }
    }
    else if (s.output)
        SDL_ClearAudioStream(s.output);
    if (audio_output && playing)
    {
        // The host owns audio pacing. Generate exactly the samples for this frame.
        int start = s.sound_sample;
        int count = static_cast<int>(std::clamp(std::lround(delta * sample_rate), 0l, static_cast<long>(sample_rate)));
        std::vector<float> mixed(static_cast<size_t>(count) * 2, 0);
        if (sound_enabled && s.programs[6])
        {
            if (!s.sound.tex)
            {
                s.sound.create(1024, 1);
                s.sound.attach();
            }
            for (int offset = 0; offset < count; offset += 1024)
            {
                bind(6, 1024, 1);
                integer(s.programs[6], "studioSampleOffset", start + offset);
                glBindFramebuffer(GL_FRAMEBUFFER, s.sound.fbo);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                std::array<float, 4096> rgba{};
                glReadPixels(0, 0, 1024, 1, GL_RGBA, GL_FLOAT, rgba.data());
                for (int n = 0; n < std::min(1024, count - offset); ++n)
                    for (int ch = 0; ch < 2; ++ch)
                        mixed[2 * (offset + n) + ch] = rgba[4 * n + ch];
            }
        }
        std::vector<std::filesystem::path> heard;
        if (sound_enabled)
            for (int i = 0; i < 7; ++i)
                for (int j = 0; j < 4 && !s.project.passes[i].source.empty(); ++j)
                {
                    const auto &c = s.project.passes[i].channels[j];
                    const auto &samples = s.assets[i][j].stereo;
                    if (c.kind != toy_input_kind::audio || samples.empty() ||
                        std::find(heard.begin(), heard.end(), c.file) != heard.end())
                        continue;
                    heard.push_back(c.file);
                    for (int n = 0; n < count; ++n)
                        for (int ch = 0; ch < 2; ++ch)
                            mixed[2 * n + ch] += samples[(static_cast<size_t>(start + n) * 2 + ch) % samples.size()];
                }
        for (auto &v : mixed)
            v = std::isfinite(v) ? std::clamp(v, -1.f, 1.f) : 0.f;
        audio_output(mixed.data(), count);
        s.sound_sample += count;
    }
    auto &out = s.buffers[5][s.front[5]];
    texture = out.tex;
    ++s.frame;
    s.time += delta;
    std::fill(keys.begin() + 256, keys.begin() + 512, 0);
    pointer[3] = -std::abs(pointer[3]);
    return out.fbo;
}
catch (const std::exception &e)
{
    media_status = e.what();
    return current ? current->buffers[5][current->front[5]].fbo : 0;
}
