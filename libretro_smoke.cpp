// A minimal libretro frontend: exercise the actual shared library and a nonzero host FBO.
#include "libretro.h"
#include "glad.h"
#include <SDL3/SDL.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    bool es = false, accept_hardware = false;
    retro_hw_render_callback hw{};
    retro_keyboard_callback keyboard{};
    GLuint fbo = 0, texture = 0;
    unsigned frames = 0;
    size_t samples = 0;
    bool nonzero_audio = false;
    std::string message;
    std::string save_directory;
    void require(bool ok, const std::string &why) { if (!ok) throw std::runtime_error(why); }
    uintptr_t RETRO_CALLCONV framebuffer() { return fbo; }
    retro_proc_address_t RETRO_CALLCONV proc(const char *name) { return reinterpret_cast<retro_proc_address_t>(SDL_GL_GetProcAddress(name)); }
    bool RETRO_CALLCONV environment(unsigned command, void *data)
    {
        switch (command)
        {
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *static_cast<const char **>(data) = save_directory.c_str(); return true;
        case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
            *static_cast<retro_hw_context_type *>(data) = es ? RETRO_HW_CONTEXT_OPENGLES3 : RETRO_HW_CONTEXT_OPENGL_CORE;
            return true;
        case RETRO_ENVIRONMENT_SET_HW_RENDER:
        {
            auto &callback = *static_cast<retro_hw_render_callback *>(data);
            require(callback.context_type == (es ? RETRO_HW_CONTEXT_OPENGLES3 : RETRO_HW_CONTEXT_OPENGL_CORE), "Wrong context requested");
            require(callback.version_major == 3 && callback.version_minor == (es ? 0u : 3u), "Wrong GL version requested");
            callback.get_current_framebuffer = framebuffer;
            callback.get_proc_address = proc;
            hw = callback;
            return accept_hardware;
        }
        case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK: keyboard = *static_cast<retro_keyboard_callback *>(data); return true;
        case RETRO_ENVIRONMENT_SET_VARIABLES: return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE:
        {
            auto &variable = *static_cast<retro_variable *>(data);
            variable.value = std::string(variable.key) == "shader_studio_editor" ? "disabled" : "960x540";
            return true;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *static_cast<bool *>(data) = false; return true;
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: return *static_cast<retro_pixel_format *>(data) == RETRO_PIXEL_FORMAT_XRGB8888;
        case RETRO_ENVIRONMENT_SET_GEOMETRY: return true;
        case RETRO_ENVIRONMENT_SET_MESSAGE: message = static_cast<retro_message *>(data)->msg; return true;
        default: return false;
        }
    }
    void RETRO_CALLCONV video(const void *data, unsigned width, unsigned height, size_t)
    {
        require(data == RETRO_HW_FRAME_BUFFER_VALID, "Missing hardware frame: " + message);
        require(width == 960 && height == 540, "Wrong frame dimensions");
        ++frames;
    }
    size_t RETRO_CALLCONV audio(const int16_t *data, size_t count)
    {
        require(count == 735, "Audio pacing is not 44100/60");
        samples += count;
        for (size_t i = 0; i < count * 2; ++i) nonzero_audio |= std::abs(data[i]) > 100;
        return count;
    }
    void RETRO_CALLCONV poll() {}
    int16_t RETRO_CALLCONV input(unsigned, unsigned, unsigned, unsigned) { return 0; }
    template<class T> T symbol(SDL_SharedObject *library, const char *name)
    {
        auto fn = reinterpret_cast<T>(SDL_LoadFunction(library, name));
        require(fn != nullptr, std::string("Missing API export: ") + name);
        return fn;
    }
    void create_target()
    {
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 960, 540, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "Host FBO incomplete");
    }
    std::array<unsigned char, 4> pixel()
    {
        std::array<unsigned char, 4> out{};
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        glReadPixels(480, 270, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
        require(glGetError() == GL_NO_ERROR, "GL error during core rendering");
        return out;
    }
}
int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    try
    {
        const auto scratch = std::filesystem::absolute(std::filesystem::u8path(argv[2]));
        std::filesystem::create_directories(scratch);
        save_directory = scratch.u8string();
        auto path = scratch / "project.stoy";
        std::ofstream(scratch / "Image.glsl") << "void mainImage(out vec4 c,in vec2 p) { c=vec4(texture(iChannel0,p/iResolution.xy).r, texelFetch(iChannel1,ivec2(65,0),0).r, 0.25,1); }";
        std::ofstream(scratch / "Buffer.glsl") << "void mainImage(out vec4 c,in vec2 p) { c=vec4(texture(iChannel0,p/iResolution.xy).r+0.125,0,0,1); }";
        std::ofstream(scratch / "Sound.glsl") << "vec2 mainSound(int s,float t) { return vec2(0.25,-0.25); }";
        std::ofstream(path) << "SHADERTOY_STUDIO 1\npass 0 \"Buffer.glsl\"\npass 5 \"Image.glsl\"\npass 6 \"Sound.glsl\"\n"
            "channel 0 0 1 \"\" 1 0 1 0 1 30\nchannel 5 0 1 \"\" 1 0 1 0 1 30\nchannel 5 1 9 \"\" 0 0 1 0 1 30\n";
        auto library = SDL_LoadObject(argv[1]);
        require(library != nullptr, SDL_GetError());
#define API(name) auto name = symbol<decltype(&::name)>(library, #name)
        API(retro_api_version); API(retro_set_environment); API(retro_set_video_refresh);
        API(retro_set_audio_sample_batch); API(retro_set_audio_sample); API(retro_set_input_poll); API(retro_set_input_state);
        API(retro_init); API(retro_deinit); API(retro_get_system_info); API(retro_get_system_av_info);
        API(retro_load_game); API(retro_unload_game); API(retro_run); API(retro_reset);
        API(retro_serialize_size); API(retro_serialize); API(retro_unserialize);
        API(retro_cheat_reset); API(retro_cheat_set); API(retro_get_region);
        API(retro_get_memory_data); API(retro_get_memory_size); API(retro_load_game_special); API(retro_set_controller_port_device);
#undef API
        require(retro_api_version() == RETRO_API_VERSION, "API version mismatch");
        retro_set_environment(environment);
        retro_set_video_refresh(video); retro_set_audio_sample_batch(audio);
        retro_set_input_poll(poll); retro_set_input_state(input);
        retro_init();
        retro_system_info info{}; retro_get_system_info(&info);
        require(info.need_fullpath && std::string(info.valid_extensions) == "stoy|glsl|frag|json", "Content is not Shadertoy-only");
        require(retro_serialize_size() == 0 && !retro_serialize(nullptr, 0) && !retro_unserialize(nullptr, 0), "Unsupported save states advertised");
        require(!retro_load_game(nullptr), "No-content load unexpectedly accepted");
        auto name = path.u8string(); retro_game_info game{name.c_str(), nullptr, 0, nullptr};
        require(!retro_load_game(&game), "Rejected hardware context was ignored");
        auto bad = (scratch / "unsupported.slangp").u8string(); game.path = bad.c_str();
        require(!retro_load_game(&game), "librashader content accepted");
        game.path = name.c_str();
        if (std::string(argv[3]) == "api")
        {
            retro_deinit(); SDL_UnloadObject(library);
            std::puts("PASS: libretro exports, Shadertoy-only content, hardware refusal and failed-load cleanup");
            return 0;
        }
        es = std::string(argv[3]) == "gles";
        require(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, es ? SDL_GL_CONTEXT_PROFILE_ES : SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, es ? 0 : 3);
        auto window = SDL_CreateWindow("libretro test", 960, 540, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        require(window != nullptr, SDL_GetError());
        auto context = SDL_GL_CreateContext(window);
        require(context != nullptr, SDL_GetError());
        require(gladLoadGLLoader(reinterpret_cast<GLADloadproc>(SDL_GL_GetProcAddress)), "Cannot load test GL functions");
        std::printf("Context: %s\n", glGetString(GL_VERSION));
        create_target();
        accept_hardware = true;
        // Compile the built-in example using the same contentless path as a frontend.
        message.clear();
        require(retro_load_game(nullptr), "Cannot load built-in example: " + message);
        hw.context_reset();
        retro_run();
        auto example = pixel();
        require(message.empty(), "Built-in example failed: " + message);
        require(example[0] || example[1] || example[2], "Built-in example rendered black");
        hw.context_destroy();
        retro_unload_game();
        samples = 0;
        nonzero_audio = false;
        require(retro_load_game(&game), message);
        hw.context_reset();
        retro_run();
        auto rgba = pixel();
        require(std::abs(rgba[0] - 32) < 3 && std::abs(rgba[2] - 64) < 3, "Multipass output not presented to frontend FBO: " + message);
        keyboard.callback(true, RETROK_a, 'a', 0);
        retro_run(); rgba = pixel();
        require(std::abs(rgba[0] - 64) < 3 && rgba[1] > 250, "Feedback or keyboard input failed");
        keyboard.callback(false, RETROK_a, 0, 0);
        require(nonzero_audio && samples == 1470, "Sound pass did not reach libretro audio");
        retro_reset(); retro_run(); rgba = pixel();
        require(std::abs(rgba[0] - 32) < 3 && rgba[1] == 0, "Reset did not clear feedback/input");
        // Open the embedded source editor and save/recompile through its keyboard action.
        keyboard.callback(true, RETROK_F10, 0, 0); keyboard.callback(false, RETROK_F10, 0, 0);
        for (int i = 0; i < 4; ++i) retro_run();
        pixel();
        // The first source tab is Buffer A. Exercise real editor text input and saving.
        keyboard.callback(true, RETROK_LCTRL, 0, RETROKMOD_CTRL);
        keyboard.callback(true, RETROK_a, 0, RETROKMOD_CTRL);
        retro_run();
        keyboard.callback(false, RETROK_a, 0, RETROKMOD_CTRL);
        keyboard.callback(false, RETROK_LCTRL, 0, 0);
        retro_run();
        const std::string edited = "void mainImage(out vec4 c,in vec2 p) { c=vec4(0.375,0,0,1); }";
        for (unsigned char c : edited) keyboard.callback(true, RETROK_UNKNOWN, c, 0);
        for (int i = 0; i < 4; ++i) retro_run();
        keyboard.callback(true, RETROK_F5, 0, 0); keyboard.callback(false, RETROK_F5, 0, 0);
        for (int i = 0; i < 4; ++i) retro_run();
        pixel();
        std::ifstream saved(scratch / "Buffer.glsl");
        std::string saved_text((std::istreambuf_iterator<char>(saved)), {});
        require(saved_text == edited, "Editor text input/save changed the shader source");
        std::ofstream(scratch / "Image.glsl") << "void mainImage(out vec4 c,in vec2 p) { broken syntax; }";
        keyboard.callback(true, RETROK_F5, 0, 0); keyboard.callback(false, RETROK_F5, 0, 0);
        for (int i = 0; i < 4; ++i) retro_run();
        pixel();
        require(message.find("Image") != message.npos, "Editor compile failure not reported");
        std::ofstream(scratch / "Image.glsl") << "void mainImage(out vec4 c,in vec2 p) { c=vec4(0.75,0.5,0.25,1); }";
        keyboard.callback(true, RETROK_F5, 0, 0); keyboard.callback(false, RETROK_F5, 0, 0);
        for (int i = 0; i < 4; ++i) retro_run();
        pixel();
        keyboard.callback(true, RETROK_F10, 0, 0); keyboard.callback(false, RETROK_F10, 0, 0);
        retro_run(); rgba = pixel();
        require(std::abs(rgba[0] - 191) < 3 && std::abs(rgba[1] - 128) < 3, "Editor rebuild did not replace output");
        hw.context_destroy();
        glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &texture);
        SDL_GL_DestroyContext(context);
        context = SDL_GL_CreateContext(window); require(context != nullptr, SDL_GetError());
        create_target(); hw.context_reset(); retro_run(); rgba = pixel();
        require(std::abs(rgba[0] - 191) < 3, "Context recreation failed");
        // Simulate context loss without a destroy callback. Reused object names must survive.
        SDL_GL_DestroyContext(context);
        context = SDL_GL_CreateContext(window); require(context != nullptr, SDL_GetError());
        create_target(); hw.context_reset(); retro_run(); rgba = pixel();
        require(std::abs(rgba[0] - 191) < 3, "Unannounced context loss deleted new frontend resources");
        hw.context_destroy(); retro_unload_game();
        require(retro_load_game(&game), "Reload failed");
        hw.context_reset(); retro_run(); pixel();
        // Content can unload before the frontend destroys its context.
        retro_unload_game(); hw.context_destroy(); retro_deinit();
        glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &texture);
        SDL_GL_DestroyContext(context); SDL_DestroyWindow(window); SDL_Quit(); SDL_UnloadObject(library);
        std::puts("PASS: multipass feedback, nonzero frontend framebuffer, input, audio, editor compile/rollback, reset, context loss and reload");
        return 0;
    }
    catch (const std::exception &e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
