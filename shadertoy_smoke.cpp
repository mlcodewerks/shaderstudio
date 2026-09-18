#include "project.h"
#include "shadertoy_runtime.h"
#include "glad.h"
#include <SDL3/SDL.h>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace
{
    void require(bool ok, const std::string &s)
    {
        if (!ok)
            throw std::runtime_error(s);
    }
    std::array<float, 4> pixel(unsigned fbo, int x = 0, int y = 0)
    {
        require(fbo != 0, "Missing Shadertoy output framebuffer");
        std::array<float, 4> rgba{};
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        glReadPixels(x, y, 1, 1, GL_RGBA, GL_FLOAT, rgba.data());
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        require(glGetError() == GL_NO_ERROR, "Shadertoy preview produced an OpenGL error");
        return rgba;
    }
    void near(float a, float b, const char *why) { require(std::abs(a - b) < 0.002f, std::string(why) + ": got " + std::to_string(a) + ", expected " + std::to_string(b)); }
    void tga(const std::filesystem::path &p, int w, int h, const std::vector<unsigned char> &rgb)
    {
        unsigned char header[18]{};
        header[2] = 2;
        header[12] = w & 255;
        header[13] = w >> 8;
        header[14] = h & 255;
        header[15] = h >> 8;
        header[16] = 24;
        header[17] = 32;
        std::ofstream out(p, std::ios::binary);
        out.write(reinterpret_cast<char *>(header), 18);
        for (size_t i = 0; i < rgb.size(); i += 3)
        {
            out.put(rgb[i + 2]);
            out.put(rgb[i + 1]);
            out.put(rgb[i]);
        }
    }
}
void shadertoy_smoke(const std::filesystem::path &root)
{
    studio_project project;
    std::string error;
    require(project.create(root / ("shadertoy-" + std::to_string(SDL_GetPerformanceCounter())), shader_template::shadertoy, error), error);
    auto &p = project.toy;
    auto directory = project.preset.parent_path();
    shadertoy_runtime runtime;
    auto write = [&](int i, const std::string &text)
    { require(shader_save_source(p.passes[i].source, text, error), error); };
    auto compile = [&]
    { require(runtime.compile(p), runtime.error); };
    compile();
    auto rgba = pixel(runtime.render(64, 32, 1.f / 60));
    require(rgba[0] > 0.5, "Shadertoy starter rendered black");
    write(5, R"(void mainImage(out vec4 c,in vec2 p) {
        c=vec4(iResolution.xy,iTime,float(iFrame));
    })");
    compile();
    rgba = pixel(runtime.render(64, 32, 0.125f));
    near(rgba[0], 64, "iResolution.x");
    near(rgba[1], 32, "iResolution.y");
    near(rgba[2], 0, "initial time");
    near(rgba[3], 0, "initial frame");
    rgba = pixel(runtime.render(64, 32, 0.125f));
    near(rgba[2], 0.125f, "iTime");
    near(rgba[3], 1, "iFrame");
    write(5, R"(void mainImage(out vec4 c,in vec2 p) { c=vec4(iTimeDelta,iFrameRate,iSampleRate,iResolution.z); })");
    compile();
    rgba = pixel(runtime.render(64, 32, 0.125f));
    near(rgba[0], 0.125f, "iTimeDelta");
    near(rgba[1], 8, "iFrameRate");
    near(rgba[2], 44100, "iSampleRate");
    near(rgba[3], 1, "pixel aspect");
    write(5, "void mainImage(out vec4 c,in vec2 p) { c=iDate; }");
    compile();
    rgba = pixel(runtime.render(64, 32, 0));
    require(rgba[0] >= 2026 && rgba[1] >= 0 && rgba[1] <= 11 && rgba[2] >= 1 && rgba[2] <= 31 && rgba[3] >= 0 && rgba[3] < 86400, "iDate fields are invalid");
    require(p.add(directory, toy_pass_kind::buffer_a, error), error);
    require(p.add(directory, toy_pass_kind::buffer_b, error), error);
    write(0, "void mainImage(out vec4 c,in vec2 p) { c=texelFetch(iChannel0,ivec2(p),0)+vec4(0.1); }");
    p.passes[0].channels[0].kind = toy_input_kind::buffer_a;
    write(1, "void mainImage(out vec4 c,in vec2 p) { c=texelFetch(iChannel0,ivec2(p),0); }");
    p.passes[1].channels[0].kind = toy_input_kind::buffer_a;
    write(5, "void mainImage(out vec4 c,in vec2 p) { c=texelFetch(iChannel0,ivec2(p),0); }");
    p.passes[5].channels[0].kind = toy_input_kind::buffer_b;
    compile();
    near(pixel(runtime.render(64, 32, 0.1f))[0], 0.1f, "same-frame dependency");
    near(pixel(runtime.render(64, 32, 0.1f))[0], 0.2f, "self feedback");
    write(0, "void mainImage(out vec4 c,in vec2 p) { c=texelFetch(iChannel0,ivec2(p),0)+vec4(0.1); }");
    p.passes[0].channels[0].kind = toy_input_kind::buffer_b;
    compile();
    near(pixel(runtime.render(64, 32, 0.1f))[0], 0.1f, "forward reference initial");
    near(pixel(runtime.render(64, 32, 0.1f))[0], 0.2f, "forward reference previous frame");
    runtime.reset();
    near(pixel(runtime.render(64, 32, 0.1f))[0], 0.1f, "restart clears feedback");
    near(pixel(runtime.render(32, 16, 0.1f))[0], 0.1f, "resize clears feedback");
    write(5, "void mainImage(out vec4 c,in vec2 p) {\n c=missing_name;\n}\n");
    require(!runtime.compile(p) && !runtime.diagnostics.entries.empty(), "Invalid GLSL did not produce inline diagnostics");
    require(runtime.diagnostics.entries[0].source == std::filesystem::weakly_canonical(p.passes[5].source) && runtime.diagnostics.entries[0].line == 2, "GLSL error line mapped incorrectly");
    near(pixel(runtime.render(32, 16, 0.1f))[0], 0.2f, "Failed compile discarded feedback");
    p.passes[0] = {};
    p.passes[1] = {};
    p.passes[5].channels = {};
    write(5, "void mainImage(out vec4 c,in vec2 p) { c=iMouse; }");
    compile();
    runtime.render(64, 32, 0);
    runtime.mouse(12, 9, true, true);
    rgba = pixel(runtime.render(64, 32, 0));
    near(rgba[0], 12, "mouse x");
    near(rgba[1], 9, "mouse y");
    near(rgba[2], 12, "click x");
    near(rgba[3], 9, "click pulse");
    rgba = pixel(runtime.render(64, 32, 0));
    near(rgba[2], 12, "mouse held");
    near(rgba[3], -9, "click pulse cleared");
    runtime.mouse(0, 0, false, false);
    rgba = pixel(runtime.render(64, 32, 0));
    near(rgba[0], 12, "released mouse position");
    near(rgba[2], -12, "mouse release sign");
    p.passes[5].channels[0].kind = toy_input_kind::keyboard;
    write(5, R"(void mainImage(out vec4 c,in vec2 p) { c=vec4(texelFetch(iChannel0,ivec2(65,0),0).r,
        texelFetch(iChannel0,ivec2(65,1),0).r,texelFetch(iChannel0,ivec2(65,2),0).r,1); })");
    compile();
    runtime.render(64, 32, 0);
    runtime.key(65, true);
    rgba = pixel(runtime.render(64, 32, 0));
    near(rgba[0], 1, "key held");
    near(rgba[1], 1, "key pulse");
    near(rgba[2], 1, "key toggle");
    runtime.key(65, true, true);
    rgba = pixel(runtime.render(64, 32, 0));
    near(rgba[1], 0, "repeat does not retrigger key");
    runtime.key(65, false);
    runtime.key(65, true);
    rgba = pixel(runtime.render(64, 32, 0));
    near(rgba[2], 0, "key toggle off");
    runtime.release_keys();
    rgba = pixel(runtime.render(64, 32, 0));
    near(rgba[0], 0, "focus loss releases keys");
    auto image = directory / "texture.tga";
    tga(image, 2, 2, {255, 0, 0, 255, 0, 0, 0, 0, 255, 0, 0, 255});
    p.passes[5].channels[0] = {};
    auto &channel = p.passes[5].channels[0];
    channel.kind = toy_input_kind::texture;
    channel.file = image;
    write(5, "void mainImage(out vec4 c,in vec2 p) { c=texelFetch(iChannel0,ivec2(0),0); }");
    compile();
    rgba = pixel(runtime.render(64, 32, 0));
    near(rgba[2], 1, "image vertical flip");
    channel.vflip = false;
    compile();
    rgba = pixel(runtime.render(64, 32, 0));
    near(rgba[0], 1, "image unflipped");
    channel.file = directory / "missing.png";
    require(!runtime.compile(p), "Missing channel image accepted");
    near(pixel(runtime.render(64, 32, 0))[0], 1, "Missing asset discarded working shader");
    channel.file = image;
    write(5, "void mainImage(out vec4 c,in vec2 p) { c=vec4(iChannelResolution[0],iChannelTime[0]); }");
    compile();
    rgba = pixel(runtime.render(64, 32, 0));
    near(rgba[0], 2, "channel width");
    near(rgba[1], 2, "channel height");
    near(rgba[2], 1, "channel depth");
    near(rgba[3], 0, "static channel time");
    auto atlas = directory / "volume.tga";
    tga(atlas, 2, 1, {255, 0, 0, 0, 255, 0});
    channel.kind = toy_input_kind::volume;
    channel.file = atlas;
    channel.depth = 2;
    channel.filter = 0;
    write(5, "void mainImage(out vec4 c,in vec2 p) { c=texture(iChannel0,vec3(0.5,0.5,0.75)); }");
    compile();
    near(pixel(runtime.render(64, 32, 0))[1], 1, "volume sampler");
    auto cube = directory / "cube.tga";
    tga(cube, 6, 1, {255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 0, 255, 0, 255, 0, 255, 255});
    channel.kind = toy_input_kind::cube_texture;
    channel.file = cube;
    write(5, "void mainImage(out vec4 c,in vec2 p) { c=texture(iChannel0,vec3(1,0,0)); }");
    compile();
    near(pixel(runtime.render(64, 32, 0))[0], 1, "cubemap texture");
    require(p.add(directory, toy_pass_kind::cubemap, error), error);
    channel.kind = toy_input_kind::cubemap;
    write(4, "void mainCubemap(out vec4 c,in vec2 p,in vec3 ro,in vec3 rd) { c=vec4(0.5+0.5*rd,1); }");
    compile();
    rgba = pixel(runtime.render(64, 32, 0));
    require(rgba[0] > 0.99 && std::abs(rgba[1] - 0.5) < 0.02, "Cubemap pass direction is incorrect");
    channel = {};
    p.passes[4] = {};
    auto seq = directory / "sequence";
    std::filesystem::create_directory(seq);
    tga(seq / "001.tga", 1, 1, {255, 0, 0});
    tga(seq / "002.tga", 1, 1, {0, 255, 0});
    channel.kind = toy_input_kind::video;
    channel.file = seq;
    channel.fps = 2;
    write(5, "void mainImage(out vec4 c,in vec2 p) { c=texture(iChannel0,vec2(0.5)); }");
    compile();
    near(pixel(runtime.render(64, 32, 0.5f))[0], 1, "video first frame");
    near(pixel(runtime.render(64, 32, 0.5f))[1], 1, "video advances");
    near(pixel(runtime.render(64, 32, 0))[0], 1, "video loops");
    if (const char *video = std::getenv("STUDIO_TEST_VIDEO"))
    {
        channel.file = std::filesystem::u8path(video);
        channel.fps = 30;
        compile();
        rgba = pixel(runtime.render(64, 32, 1.f / 30));
        require(rgba[0] > 0.8 && rgba[1] < 0.1, "FFmpeg video first frame did not decode");
        for (int i = 0; i < 20; ++i)
        {
            SDL_Delay(10);
            rgba = pixel(runtime.render(64, 32, 1.f / 30));
        }
        require(rgba[1] > 0.8 && rgba[0] < 0.1, "FFmpeg video did not advance to the next frame");
        runtime.reset();
        rgba = pixel(runtime.render(64, 32, 0));
        for (int i = 0; i < 100 && rgba[0] < 0.8; ++i)
        {
            SDL_Delay(10);
            rgba = pixel(runtime.render(64, 32, 0));
        }
        require(rgba[0] > 0.8, "FFmpeg video did not restart");
    }
    auto wav = directory / "audio.wav";
    {
        std::ofstream f(wav, std::ios::binary);
        auto le = [&](unsigned n, int count)
        { for (int i=0;i<count;++i) f.put(static_cast<char>((n>>(8*i))&255)); };
        f.write("RIFF", 4);
        le(36 + 44100 * 2, 4);
        f.write("WAVEfmt ", 8);
        le(16, 4);
        le(1, 2);
        le(1, 2);
        le(44100, 4);
        le(88200, 4);
        le(2, 2);
        le(16, 2);
        f.write("data", 4);
        le(44100 * 2, 4);
        for (int i = 0; i < 44100; ++i)
            le(16384, 2);
    }
    channel = {};
    channel.kind = toy_input_kind::audio;
    channel.file = wav;
    write(5, "void mainImage(out vec4 c,in vec2 p) { c=vec4(texelFetch(iChannel0,ivec2(256,1),0).r,iChannelResolution[0].xy,iChannelTime[0]); }");
    compile();
    runtime.sound_enabled = true;
    runtime.render(64, 32, 0.1f);
    rgba = pixel(runtime.render(64, 32, 0.1f));
    require(std::abs(rgba[0] - 0.75f) < 0.01, "Audio waveform texture is incorrect");
    near(rgba[1], 512, "audio texture width");
    near(rgba[2], 2, "audio texture height");
    near(rgba[3], 0.1f, "audio channel time");
    require(runtime.media_status.empty(), runtime.media_status);
    channel = {};
    p.vr = true;
    write(5, "void mainVR(out vec4 c,in vec2 p,in vec3 ro,in vec3 rd) { c=vec4(abs(rd),1); }");
    compile();
    require(pixel(runtime.render(64, 32, 0), 32, 16)[2] > 0.99, "mainVR preview did not run");
    p.vr = false;
    require(p.add(directory, toy_pass_kind::sound, error), error);
    write(5, toy_template(toy_pass_kind::image));
    compile();
    runtime.sound_enabled = true;
    runtime.render(64, 32, 1.f / 60);
    require(runtime.media_status.empty(), runtime.media_status);
    runtime.set_playing(false);
    runtime.render(64, 32, 0);
    runtime.set_playing(true);
    require(shader_save_source(p.common, "float commonValue() {\n return missing_common;\n}\n", error), error);
    require(!runtime.compile(p) && !runtime.diagnostics.entries.empty(), "Common syntax error not reported");
    require(runtime.diagnostics.entries[0].source == std::filesystem::weakly_canonical(p.common) && runtime.diagnostics.entries[0].line == 2,
     "Common error mapped to wrong source");
    require(shader_save_source(p.common, "// Shared functions\n", error), error);
    compile();
    require(runtime.diagnostics.entries.empty(), "Corrected source retained diagnostics");
    require(project.save(project.preset, error), error);
    auto json = directory / "import-test.json";
    require(shader_save_source(json, R"({"Shader":{"info":{"name":"Imported feedback"},"renderpass":[
        {"type":"image","code":"void mainImage(out vec4 c,in vec2 p) { c=texelFetch(iChannel2,ivec2(p),0); }",
         "inputs":[{"channel":2,"ctype":"buffer","id":"feedback"}]},
        {"type":"common","code":"const float increment = 0.25;"},
        {"type":"buffer","name":"Buffer A","outputs":[{"id":"feedback","channel":0}],
         "inputs":[{"channel":0,"ctype":"buffer","id":"feedback","sampler":{"filter":"nearest","wrap":"clamp","vflip":"false","srgb":"false"}}],
         "code":"void mainImage(out vec4 c,in vec2 p) { c=texelFetch(iChannel0,ivec2(p),0)+vec4(increment); }"}
    ]}})",
                               error),
            error);
    studio_project imported;
    require(imported.load(json, error), error);
    require(runtime.compile(imported.toy), runtime.error);
    near(pixel(runtime.render(64, 32, 1.f / 60))[0], 0.25f, "JSON Common and buffer input");
    near(pixel(runtime.render(64, 32, 1.f / 60))[0], 0.5f, "JSON feedback graph");
    std::puts("PASS: imported Shadertoy JSON compiles and renders Common, channel slots and buffer feedback");
    std::puts("PASS: Shadertoy uniforms, date, feedback/order/reset/resize, diagnostics/rollback,"\ 
        " mouse/keyboard, textures/volume/cubemap, video sequence, "\
         "WAV analysis/playback, mainVR, sound and project save");
}
