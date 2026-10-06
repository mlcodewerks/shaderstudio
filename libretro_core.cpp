#include "libretro.h"
#include "glad.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "ImGuiFileDialog.h"
#include "project.h"
#include "preview.h"
#include "workspace.h"
#include "shadertoy_download.h"
#include "shader_source_editor.h"
#include "shadertoy_runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

void studio_imgui_abandon_context();

unsigned shader_gl_render(unsigned source_fbo, unsigned iw, unsigned ih, unsigned ow, unsigned oh)
{
    return 0;
}

namespace
{
    retro_environment_t environment = nullptr;
    retro_video_refresh_t video = nullptr;
    retro_audio_sample_t audio = nullptr;
    retro_audio_sample_batch_t audio_batch = nullptr;
    retro_input_poll_t input_poll = nullptr;
    retro_input_state_t input_state = nullptr;
    retro_log_printf_t logger = nullptr;
    retro_hw_render_callback hardware{};

    ImGuiContext *ui = nullptr;
    bool loaded = false;
    bool context = false;
    bool backend = false;
    bool show_editor = true;
    bool reset_pending = false;
    bool last_l3 = false;
    bool last_mouse = false;
    bool audio_submitted = false;
    unsigned width = 1280;
    unsigned height = 720;
    float mouse_x = 640.f;
    float mouse_y = 360.f;
    std::string reported;
    std::string editor_option;
    std::array<bool, RETROK_LAST> key_down{};

    void report(const std::string &message)
    {
        if (message.empty() || message == reported)
            return;
        reported = message;
        if (logger)
            logger(RETRO_LOG_ERROR, "[Shader Studio] %s\n", message.c_str());
        retro_message notification{message.c_str(), 240};
        if (environment)
            environment(RETRO_ENVIRONMENT_SET_MESSAGE, &notification);
    }

    struct ui_scope
    {
        ImGuiContext *previous = ImGui::GetCurrentContext();
        ImGuiContext *owned = ui;
        ui_scope() { ImGui::SetCurrentContext(ui); }
        ~ui_scope() { ImGui::SetCurrentContext(previous == owned ? nullptr : previous); }
    };

    int input_state_value(unsigned device, unsigned id)
    {
        return input_state ? input_state(0, device, 0, id) : 0;
    }

    void submit_audio(const float *samples, size_t frames)
    {
        audio_submitted = true;
        std::vector<int16_t> pcm(frames * 2);
        for (size_t n = 0; n < pcm.size(); ++n)
            pcm[n] = static_cast<int16_t>(std::lround(std::clamp(samples[n], -1.f, 1.f) * 32767));
        if (audio_batch)
            audio_batch(pcm.data(), frames);
        else if (audio)
            for (size_t n = 0; n < frames; ++n)
                audio(pcm[n * 2], pcm[n * 2 + 1]);
    }

    void silence()
    {
        std::array<float, 735 * 2> samples{};
        submit_audio(samples.data(), 735);
    }

    bool input(const char *label, std::string &value, ImGuiInputTextFlags flags = 0)
    {
        char text[4096]{};
        std::memcpy(text, value.data(), std::min(value.size(), sizeof(text) - 1));
        if (!ImGui::InputText(label, text, sizeof(text), flags))
            return false;
        value = text;
        return true;
    }

    ImGuiKey imgui_key(unsigned code)
    {
        if (code >= RETROK_a && code <= RETROK_z)
            return static_cast<ImGuiKey>(ImGuiKey_A + code - RETROK_a);
        if (code >= RETROK_0 && code <= RETROK_9)
            return static_cast<ImGuiKey>(ImGuiKey_0 + code - RETROK_0);
        if (code >= RETROK_F1 && code <= RETROK_F12)
            return static_cast<ImGuiKey>(ImGuiKey_F1 + code - RETROK_F1);
        switch (code)
        {
        case RETROK_LEFT: return ImGuiKey_LeftArrow;
        case RETROK_RIGHT: return ImGuiKey_RightArrow;
        case RETROK_UP: return ImGuiKey_UpArrow;
        case RETROK_DOWN: return ImGuiKey_DownArrow;
        case RETROK_RETURN: return ImGuiKey_Enter;
        case RETROK_KP_ENTER: return ImGuiKey_KeypadEnter;
        case RETROK_ESCAPE: return ImGuiKey_Escape;
        case RETROK_TAB: return ImGuiKey_Tab;
        case RETROK_BACKSPACE: return ImGuiKey_Backspace;
        case RETROK_DELETE: return ImGuiKey_Delete;
        case RETROK_INSERT: return ImGuiKey_Insert;
        case RETROK_HOME: return ImGuiKey_Home;
        case RETROK_END: return ImGuiKey_End;
        case RETROK_PAGEUP: return ImGuiKey_PageUp;
        case RETROK_PAGEDOWN: return ImGuiKey_PageDown;
        case RETROK_SPACE: return ImGuiKey_Space;
        case RETROK_LCTRL: return ImGuiKey_LeftCtrl;
        case RETROK_RCTRL: return ImGuiKey_RightCtrl;
        case RETROK_LSHIFT: return ImGuiKey_LeftShift;
        case RETROK_RSHIFT: return ImGuiKey_RightShift;
        case RETROK_LALT: return ImGuiKey_LeftAlt;
        case RETROK_RALT: return ImGuiKey_RightAlt;
        default: return ImGuiKey_None;
        }
    }

    unsigned toy_key(unsigned code)
    {
        if (code >= RETROK_a && code <= RETROK_z)
            return static_cast<unsigned>(code - RETROK_a + 65);
        if (code >= RETROK_0 && code <= RETROK_9)
            return static_cast<unsigned>(code);
        if (code >= RETROK_F1 && code <= RETROK_F12)
            return static_cast<unsigned>(code - RETROK_F1 + 112);
        switch (code)
        {
        case RETROK_BACKSPACE: return 8;
        case RETROK_TAB: return 9;
        case RETROK_RETURN:
        case RETROK_KP_ENTER: return 13;
        case RETROK_LSHIFT:
        case RETROK_RSHIFT: return 16;
        case RETROK_LCTRL:
        case RETROK_RCTRL: return 17;
        case RETROK_LALT:
        case RETROK_RALT: return 18;
        case RETROK_ESCAPE: return 27;
        case RETROK_SPACE: return 32;
        case RETROK_PAGEUP: return 33;
        case RETROK_PAGEDOWN: return 34;
        case RETROK_END: return 35;
        case RETROK_HOME: return 36;
        case RETROK_LEFT: return 37;
        case RETROK_UP: return 38;
        case RETROK_RIGHT: return 39;
        case RETROK_DOWN: return 40;
        case RETROK_INSERT: return 45;
        case RETROK_DELETE: return 46;
        case RETROK_SEMICOLON: return 186;
        case RETROK_EQUALS: return 187;
        case RETROK_COMMA: return 188;
        case RETROK_MINUS: return 189;
        case RETROK_PERIOD: return 190;
        case RETROK_SLASH: return 191;
        case RETROK_BACKQUOTE: return 192;
        case RETROK_LEFTBRACKET: return 219;
        case RETROK_BACKSLASH: return 220;
        case RETROK_RIGHTBRACKET: return 221;
        case RETROK_QUOTE: return 222;
        default: return 0;
        }
    }

    bool validate_project(const studio_project &candidate, std::string &error)
    {
        if (!candidate.is_shadertoy)
        {
            error = "The libretro core supports Shadertoy projects only.";
            return false;
        }
        for (const auto &pass : candidate.toy.passes)
            for (const auto &channel : pass.channels)
                if (!pass.source.empty() &&
                    (channel.kind == toy_input_kind::microphone || channel.kind == toy_input_kind::camera))
                {
                    error = "Live camera and microphone inputs are not available in the libretro core.";
                    return false;
                }
        return true;
    }

    class studio
    {
    public:
        studio_project project;
        studio_preview preview;
        shadertoy_runtime toy_runtime;
        shader_source_editor editor;
        unsigned last_fbo = 0;

        studio()
        {
            // preview GL resources are created from context_reset(), after the frontend
            // has supplied a current OpenGL context and function table.
            new_directory = (std::filesystem::current_path() / "my-shader").u8string();
            url_directory = (std::filesystem::current_path() / "shadertoy-download").u8string();
            toy_runtime.audio_output = submit_audio;
            toy_runtime.sound_enabled = true;
        }

        ~studio()
        {
            download_cancel = true;
            if (download.valid())
                download.wait();
        }

        void initialize_paths_from_project()
        {
            if (!project.preset.empty())
            {
                new_directory = (project.preset.parent_path() / "my-shader").u8string();
                url_directory = (project.preset.parent_path() / "shadertoy-download").u8string();
            }
        }

        void open_url(const std::string &url = {})
        {
            action([this, url]
                   { if (!url.empty()) url_input = url; url_popup = true; });
        }

        void compile()
        {
            project.sync_toy();
            if (!toy_runtime.compile(project.toy))
                report(toy_runtime.error);
            else
                reported.clear();
            force_frame = true;
        }

        void open_editor()
        {
            editor.open_preset(project.chain, status);
        }

        bool create(const std::filesystem::path &path, shader_template kind)
        {
            if (kind != shader_template::shadertoy)
            {
                status = "The libretro core supports Shadertoy projects only.";
                return false;
            }
            if (!project.create(path, kind, status))
                return false;
            reset_editor();
            compile();
            open_editor();
            return true;
        }

        bool load(const std::filesystem::path &path)
        {
            studio_project next;
            if (!next.load(path, status))
                return false;
            if (!validate_project(next, status))
                return false;
            project = std::move(next);
            initialize_paths_from_project();
            reset_editor();
            compile();
            open_editor();
            return true;
        }

        void key(bool down, unsigned code, bool repeat)
        {
            if (down && (!preview_focused || ImGui::GetIO().WantTextInput))
                return;
            const unsigned mapped = toy_key(code);
            if (mapped)
                toy_runtime.key(mapped, down, repeat);
        }

        void release_keys()
        {
            toy_runtime.release_keys();
            preview_focused = false;
            mouse_drag = false;
        }

        void restart()
        {
            toy_runtime.reset();
            preview.output_texture = 0;
            force_frame = true;
        }

        void release_preview_context()
        {
            // Libretro can destroy/recreate its frontend-owned GL context.
            // Release preview-owned names while that context is still current.
            preview.destroy();
        }

        void restore_preview_context()
        {
            if (!preview_image.empty())
            {
                if (!preview.load_image(preview_image, status))
                {
                    preview_image.clear();
                    preview.test_card();
                }
            }
            else
                preview.test_card();
            force_frame = true;
        }

        void draw_fullscreen(unsigned render_width, unsigned render_height,
                             float x, float y, bool down, bool clicked)
        {
            toy_runtime.set_playing(!paused);
            toy_runtime.mouse(x, render_height - y, down, clicked);
            if (force_frame || !paused || !toy_runtime.texture)
            {
                last_fbo = toy_runtime.render(render_width, render_height, 1.f / 60.f);
                force_frame = false;
            }
        }

        void draw()
        {
            if (download.valid() && download.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            {
                auto result = download.get();
                if (result.ok && !download_cancel)
                {
                    if (!validate_project(result.project, result.error))
                    {
                        status = result.error;
                        url_error = status;
                    }
                    else
                    {
                        project = std::move(result.project);
                        initialize_paths_from_project();
                        reset_editor();
                        compile();
                        open_editor();
                        status = "Shadertoy project downloaded. See JSON import notes for any assets that need attention.";
                        close_url_popup = true;
                    }
                }
                else
                {
                    status = result.error.empty() ? "Download cancelled." : result.error;
                    url_error = status;
                }
            }

            if (ImGui::BeginMainMenuBar())
            {
                if (ImGui::BeginMenu("Project"))
                {
                    if (ImGui::MenuItem("New project..."))
                        action([this]
                               { new_popup = true; });
                    if (ImGui::MenuItem("Open preset / shader..."))
                        action([this]
                               { pick(picker_type::open, "Open project or shader", ".stoy,.glsl,.frag,.json"); });
                    if (ImGui::MenuItem("Open Shadertoy URL..."))
                        open_url();
                    if (ImGui::MenuItem("Save project", nullptr, false, !project.chain.passes.empty()))
                        save_project();
                    if (ImGui::MenuItem("Save preset as...", nullptr, false, !project.chain.passes.empty()))
                        pick(picker_type::save, "Save shader project", ".stoy");
                    ImGui::EndMenu();
                }
                if (ImGui::MenuItem("Save all and compile", "F5", false, !project.chain.passes.empty()))
                    save_compile();
                if (ImGui::MenuItem("Edit shaders", nullptr, false, !project.chain.passes.empty()))
                    open_editor();
                ImGui::EndMainMenuBar();
            }

            workspace.draw(editor.is_visible());
            if (ImGui::IsKeyPressed(ImGuiKey_F5, false) && picker == picker_type::none && !pending && !download.valid())
                save_compile();

            ImGui::SetNextWindowPos(workspace.project_pos);
            ImGui::SetNextWindowSize(workspace.project_size);
            if (ImGui::Begin("Shader project", nullptr, studio_workspace::flags))
            {
                ImGui::TextUnformatted(project.chain.name.empty() ? "Create your first shader" : project.chain.name.c_str());
                if (!project.preset.empty())
                    ImGui::TextWrapped("%s", project.preset.u8string().c_str());
                if (project.dirty || editor.has_unsaved_changes())
                    ImGui::TextUnformatted("Unsaved project changes");
                if (!status.empty())
                    ImGui::TextWrapped("%s", status.c_str());

                ImGui::BeginDisabled(picker != picker_type::none || static_cast<bool>(pending));
                if (ImGui::Button("New project..."))
                    action([this]
                           { new_popup = true; });
                ImGui::SameLine();
                if (ImGui::Button("Open..."))
                    action([this]
                           { pick(picker_type::open, "Open project or shader", ".stoy,.glsl,.frag,.json"); });
                if (ImGui::Button("Open Shadertoy URL..."))
                    open_url();

                if (!project.chain.passes.empty())
                {
                    if (ImGui::Button("Save project"))
                        save_project();
                    ImGui::SameLine();
                    if (ImGui::Button("Edit all passes"))
                        open_editor();
                    draw_toy_passes();
                }
                else
                {
                    ImGui::TextWrapped("Create a Shadertoy project. Edit GLSL and press F5 to save and compile while the preview runs.");
                }
                ImGui::EndDisabled();
            }
            ImGui::End();

            draw_preview();

            if (editor.is_visible())
            {
                ImGui::SetNextWindowPos(workspace.editor_pos);
                if (editor.draw(toy_runtime.error, toy_runtime.diagnostics, workspace.editor_size, "GLSL / Slang", studio_workspace::flags))
                    compile();
            }

            dialogs();
        }

        void close_dialogs_for_unload()
        {
            download_cancel = true;
            if (download.valid())
            {
                download.wait();
                download = std::future<download_result>{};
            }
            if (picker != picker_type::none)
                ImGuiFileDialog::Instance()->Close();
            picker = picker_type::none;
            pending = {};
            new_popup = false;
            url_popup = false;
            close_url_popup = false;
        }

    private:
        enum class picker_type
        {
            none,
            open,
            save,
            pass,
            image,
            toy_asset,
            texture
        } picker = picker_type::none;

        std::string status;
        std::string new_directory;

        struct download_result
        {
            studio_project project;
            std::string error;
            bool ok = false;
        };

        std::future<download_result> download;
        std::atomic_bool download_cancel{false};
        std::string url_input;
        std::string url_key;
        std::string url_directory;
        std::string url_error;
        bool url_popup = false;
        bool close_url_popup = false;
        bool url_assets = true;
        int template_index = 0;
        int output_width = 640;
        int output_height = 480;
        bool new_popup = false;
        bool paused = false;
        bool original = false;
        bool force_frame = true;
        bool preview_focused = false;
        bool mouse_drag = false;
        int asset_pass = 0;
        int asset_channel = 0;
        std::filesystem::path preview_image;
        studio_workspace workspace;
        std::function<void()> pending;

        shader_template template_kind() const { return shader_template::shadertoy; }
        bool unsaved() const { return project.dirty || editor.has_unsaved_changes(); }
        void reset_editor() { editor = shader_source_editor{}; }



        void action(std::function<void()> next)
        {
            if (pending || download.valid())
                return;
            if (unsaved())
                pending = std::move(next);
            else
                next();
        }

        void pick(picker_type type, const char *title, const char *filter)
        {
            if (picker != picker_type::none)
                return;
            picker = type;
            auto directory = project.preset.empty() ? std::filesystem::current_path() : project.preset.parent_path();
            ImGuiFileDialog::Instance()->OpenDialog("StudioPicker", title, filter, directory.u8string(), "", 1, nullptr,
                                                    type == picker_type::save ? ImGuiFileDialogFlags_ConfirmOverwrite : ImGuiFileDialogFlags_None);
        }

        void save_compile()
        {
            if (project.chain.passes.empty())
                return;
            if (editor.save_all(status))
                compile();
        }

        bool save_project()
        {
            if (project.preset.empty())
            {
                pick(picker_type::save, "Save shader project", ".stoy");
                return false;
            }
            if (!editor.save_all(status) || !project.save(project.preset, status))
                return false;
            compile();
            status = "Project saved.";
            return true;
        }

        void changed()
        {
            project.dirty = true;
            compile();
        }

        void draw_toy_passes()
        {
            ImGui::TextWrapped("Native Shadertoy GLSL: Image, Common, buffers, cubemap and sound.");
            if (!project.toy.import_notes.empty() && ImGui::TreeNode("JSON import notes"))
            {
                for (const auto &note : project.toy.import_notes)
                    ImGui::TextWrapped("%s", note.c_str());
                ImGui::TreePop();
            }

            auto directory = project.preset.empty() ? project.toy.passes[5].source.parent_path() : project.preset.parent_path();
            if (project.toy.common.empty() && ImGui::Button("Add Common"))
            {
                auto file = directory / "Common.glsl";
                if (std::filesystem::exists(file))
                    status = "Common.glsl already exists; choose a project with a Common tab.";
                else if (shader_save_source(file, "// Shared GLSL functions and constants.\n", status))
                {
                    project.toy.common = file;
                    changed();
                    open_editor();
                }
            }

            for (int i = 0; i < 7; ++i)
            {
                ImGui::PushID(i);
                auto &p = project.toy.passes[i];
                auto kind = static_cast<toy_pass_kind>(i);
                if (p.source.empty())
                {
                    if (ImGui::Button((std::string("Add ") + toy_pass_name(kind)).c_str()) && project.toy.add(directory, kind, status))
                    {
                        changed();
                        open_editor();
                    }
                }
                else if (ImGui::TreeNode(toy_pass_name(kind)))
                {
                    if (ImGui::SmallButton("Edit"))
                    {
                        editor.open(p.source, status);
                    }
                    if (i != 5)
                    {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Remove pass"))
                        {
                            p = {};
                            for (auto &other : project.toy.passes)
                                for (auto &c : other.channels)
                                    if (static_cast<int>(c.kind) == i + 1)
                                        c.kind = toy_input_kind::none;
                            changed();
                            open_editor();
                            ImGui::TreePop();
                            ImGui::PopID();
                            continue;
                        }
                    }

                    for (int j = 0; j < 4; ++j)
                    {
                        ImGui::PushID(j);
                        auto &c = p.channels[j];
                        bool edited = false;
                        ImGui::Separator();
                        ImGui::Text("iChannel%d", j);
                        if (ImGui::BeginCombo("Input", toy_input_name(c.kind)))
                        {
                            for (int k = 0; k <= 13; ++k)
                            {
                                auto type = static_cast<toy_input_kind>(k);
                                if (type == toy_input_kind::microphone || type == toy_input_kind::camera)
                                    continue;
                                bool missing = k >= 1 && k <= 5 && project.toy.passes[k - 1].source.empty();
                                ImGui::BeginDisabled(missing);
                                if (ImGui::Selectable(toy_input_name(type), c.kind == type))
                                {
                                    c.kind = type;
                                    edited = true;
                                }
                                ImGui::EndDisabled();
                            }
                            ImGui::EndCombo();
                        }

                        bool file_input = c.kind == toy_input_kind::texture || c.kind == toy_input_kind::cube_texture ||
                                          c.kind == toy_input_kind::volume || c.kind == toy_input_kind::audio || c.kind == toy_input_kind::video;
                        if (file_input)
                        {
                            if (!c.origin.empty())
                                ImGui::TextWrapped("Imported asset: %s", c.origin.c_str());
                            auto file = c.file.u8string();
                            if (input(c.kind == toy_input_kind::video ? "Video / frame folder" : "File", file))
                            {
                                c.file = file.empty() ? std::filesystem::path{} : (directory / std::filesystem::u8path(file)).lexically_normal();
                                edited = true;
                            }
                            if (ImGui::SmallButton("Browse..."))
                            {
                                asset_pass = i;
                                asset_channel = j;
                                pick(picker_type::toy_asset, "Choose channel asset", c.kind == toy_input_kind::audio ? ".wav,.flac,.ogg,.oga,.mp3,.opus,.aac,.m4a,.weba,.mka,.mod,.s3m,.xm,.ac3,.eac3,.ec3" :
                                    c.kind == toy_input_kind::video ? ".mp4,.m4v,.mov,.webm,.mkv"
                                    : ".png,.jpg,.jpeg,.bmp,.tga");
                            }
                            if (c.kind == toy_input_kind::cube_texture)
                                ImGui::TextWrapped("Six square faces in a horizontal strip: +X, -X, +Y, -Y, +Z, -Z.");
                            if (c.kind == toy_input_kind::volume)
                            {
                                edited |= ImGui::InputInt("Slices", &c.depth);
                                c.depth = std::clamp(c.depth, 1, 256);
                                ImGui::TextWrapped("Horizontal atlas of equally sized Z slices.");
                            }
                            if (c.kind == toy_input_kind::video)
                            {
                                edited |= ImGui::InputFloat("Frames per second", &c.fps);
                                c.fps = std::clamp(c.fps, 1.f, 240.f);
                                ImGui::TextWrapped("MP4 / WebM video loops at its recorded frame timing. FPS is the fallback for missing timestamps and numbered image folders.");
                            }
                        }

                        if (c.kind != toy_input_kind::none)
                        {
                            edited |= ImGui::Combo("Filter", &c.filter, "Nearest\0Linear\0Mipmaps\0");
                            edited |= ImGui::Combo("Wrap", &c.wrap, "Clamp\0Repeat\0");
                            if (file_input && c.kind != toy_input_kind::audio)
                            {
                                edited |= ImGui::Checkbox("Flip vertically", &c.vflip);
                                edited |= ImGui::Checkbox("sRGB decode", &c.srgb);
                            }
                        }
                        project.dirty |= edited;
                        ImGui::PopID();
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }

            if (ImGui::Checkbox("mainVR desktop camera", &project.toy.vr))
                project.dirty = true;
            if (ImGui::Button("Apply channels and compile"))
                save_compile();
            ImGui::TextWrapped("Uniforms: iResolution, iTime, iTimeDelta, iFrame, iFrameRate, iMouse,"
                "iDate, iSampleRate, iChannel0..3, iChannelTime[4], iChannelResolution[4].");
        }

        void draw_preview()
        {
            workspace.prepare_preview();
            if (ImGui::Begin("Live preview", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
            {
                workspace.capture_preview();
                workspace.begin_controls();
                if (ImGui::Button("Load image..."))
                    pick(picker_type::image, "Load preview image", ".png,.jpg,.jpeg,.bmp,.tga");
                studio_workspace::next_control("Test card", false);
                if (ImGui::Button("Test card"))
                {
                    preview.test_card();
                    preview_image.clear();
                    force_frame = true;
                }
                studio_workspace::next_control("Pause", true);
                ImGui::Checkbox("Pause", &paused);
                studio_workspace::next_control("Step", false);
                if (ImGui::Button("Step"))
                {
                    paused = true;
                    force_frame = true;
                }
                studio_workspace::next_control("Restart", false);
                if (ImGui::Button("Restart"))
                {
                    toy_runtime.reset();
                    preview.output_texture = 0;
                    force_frame = true;
                }
                studio_workspace::next_control("Original", true);
                ImGui::Checkbox("Original", &original);

                if (ImGui::Checkbox("Play audio", &toy_runtime.sound_enabled))
                    toy_runtime.media_status.clear();
                toy_runtime.set_playing(!paused);
                ImGui::TextWrapped("Time %.3f s | Frame %d | Click preview for keyboard input", toy_runtime.time(), toy_runtime.frame());

                ImGui::Text("Resolution %d x %d", output_width, output_height);
                if (!toy_runtime.media_status.empty())
                    ImGui::TextWrapped("%s", toy_runtime.media_status.c_str());
                if (!toy_runtime.error.empty())
                    ImGui::TextWrapped("Compilation failed: the previous working preview is retained. See the source editor for diagnostics.");

                workspace.end_controls();
                const auto available = ImGui::GetContentRegionAvail();
                workspace.measure_preview(ImGui::GetWindowSize(), available);
                const int new_width = std::clamp(static_cast<int>(available.x), 16, 4096);
                const int new_height = std::clamp(static_cast<int>(available.y), 16, 4096);
                force_frame |= new_width != output_width || new_height != output_height;
                output_width = new_width;
                output_height = new_height;
                // The frontend supplies the 60 Hz clock, including during pane drags.
                if (force_frame || !paused)
                {
                    last_fbo = toy_runtime.render(output_width, output_height, 1.f / 60.f);
                    preview.output_texture = toy_runtime.texture;
                    force_frame = false;
                }

                const float aspect = original ? static_cast<float>(preview.width) / preview.height :
                    static_cast<float>(output_width) / output_height;
                float w = std::max(1.f, std::min(available.x, available.y * aspect));
                auto texture = original || !preview.output_texture ? preview.input_texture : preview.output_texture;
                auto top_left = ImGui::GetCursorScreenPos();
                bool flip = !original && preview.output_texture;
                if (texture)
                    ImGui::Image(static_cast<ImTextureID>(texture), ImVec2(w, w / aspect),
                                 flip ? ImVec2(0, 1) : ImVec2(0, 0),
                                 flip ? ImVec2(1, 0) : ImVec2(1, 1));
                else
                    ImGui::Dummy(ImVec2(w, w / aspect));

                bool clicked = ImGui::IsItemHovered() && ImGui::IsMouseClicked(0);
                if (ImGui::IsMouseClicked(0))
                {
                    preview_focused = clicked;
                    if (!clicked)
                        toy_runtime.release_keys();
                }
                if (clicked)
                    mouse_drag = true;
                bool down = mouse_drag && ImGui::IsMouseDown(0);
                auto pos = ImGui::GetMousePos();
                toy_runtime.mouse(std::clamp((pos.x - top_left.x) / w, 0.f, 1.f) * output_width,
                                  std::clamp(1.f - (pos.y - top_left.y) / (w / aspect), 0.f, 1.f) * output_height,
                                  down, clicked);
                if (!down)
                    mouse_drag = false;

            }
            ImGui::End();
        }

        void dialogs()
        {
            if (url_popup)
            {
                ImGui::OpenPopup("Open Shadertoy URL");
                url_popup = false;
                url_error.clear();
            }
            if (ImGui::BeginPopupModal("Open Shadertoy URL", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::BeginDisabled(download.valid());
                ImGui::SetNextItemWidth(540);
                input("URL or shader ID", url_input);
                ImGui::SetNextItemWidth(540);
                input("New download folder", url_directory);
                ImGui::SetNextItemWidth(540);
                input("API key", url_key, ImGuiInputTextFlags_Password);
                ImGui::Checkbox("Download supported Shadertoy media", &url_assets);
                ImGui::TextWrapped("An API key uses the Public+API endpoint.\n"
                    "The download creates an editable project in a new folder."
                    "API keys are kept only for this session.");
                if (ImGui::Button("Download and open"))
                {
                    std::string id;
                    if (studio_shadertoy_id(url_input, id, url_error) && !url_directory.empty())
                    {
                        download_cancel = false;
                        const auto url = url_input, key = url_key, directory = url_directory;
                        const bool assets = url_assets;
                        download = std::async(std::launch::async, [this, url, key, directory, assets]
                                              {
                            download_result result;
                            try
                            {
                                result.ok = studio_download_shadertoy(url, key, std::filesystem::u8path(directory), assets,
                                    result.project, download_cancel, result.error);
                            }
                            catch (const std::exception &e)
                            {
                                result.error = e.what();
                            }
                            return result;
                        });
                    }
                    else if (url_directory.empty())
                        url_error = "Enter a new download folder.";
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button(download.valid() ? "Cancel download" : "Close"))
                {
                    if (download.valid())
                        download_cancel = true;
                    else
                        ImGui::CloseCurrentPopup();
                }
                if (download.valid())
                    ImGui::TextUnformatted(download_cancel ? "Cancelling download..." : "Downloading shader and media...");
                if (!url_error.empty())
                    ImGui::TextWrapped("%s", url_error.c_str());
                if (close_url_popup)
                {
                    close_url_popup = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            if (new_popup)
            {
                ImGui::OpenPopup("New shader project");
                new_popup = false;
            }
            if (ImGui::BeginPopupModal("New shader project", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::SetNextItemWidth(540);
                input("New folder", new_directory);
                ImGui::Combo("Starter shader", &template_index, "Shadertoy\0");
                if (!status.empty())
                    ImGui::TextWrapped("%s", status.c_str());
                if (ImGui::Button("Create") && !new_directory.empty() && create(std::filesystem::u8path(new_directory), template_kind()))
                    ImGui::CloseCurrentPopup();
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }

            if (pending)
                ImGui::OpenPopup("Unsaved project changes");
            if (ImGui::BeginPopupModal("Unsaved project changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::TextUnformatted("Save source and preset changes before continuing?");
                if (project.preset.empty())
                    ImGui::TextUnformatted("Cancel and use Save preset as to choose a preset filename.");
                if (!status.empty())
                    ImGui::TextWrapped("%s", status.c_str());
                bool proceed = false;
                ImGui::BeginDisabled(project.preset.empty());
                if (ImGui::Button("Save and continue") && save_project())
                    proceed = true;
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("Discard changes"))
                    proceed = true;
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                {
                    pending = {};
                    ImGui::CloseCurrentPopup();
                }
                if (proceed)
                {
                    auto next = std::move(pending);
                    pending = {};
                    ImGui::CloseCurrentPopup();
                    next();
                }
                ImGui::EndPopup();
            }

            const auto size = ImGui::GetMainViewport()->WorkSize;
            if (picker != picker_type::none && ImGuiFileDialog::Instance()->Display("StudioPicker", ImGuiWindowFlags_NoCollapse,
                ImVec2(std::min(640.f, size.x), std::min(400.f, size.y)),
                ImVec2(size.x * 0.95f, size.y * 0.95f)))
            {
                if (ImGuiFileDialog::Instance()->IsOk())
                {
                    auto path = std::filesystem::u8path(ImGuiFileDialog::Instance()->GetFilePathName());
                    if (picker == picker_type::open)
                    {
                        studio_project next;
                        if (next.load(path, status) && validate_project(next, status))
                        {
                            project = std::move(next);
                            initialize_paths_from_project();
                            reset_editor();
                            compile();
                            open_editor();
                        }
                    }
                    else if (picker == picker_type::save && editor.save_all(status) && project.save(path, status))
                    {
                        compile();
                        status = "Project saved.";
                    }
                    else if (picker == picker_type::image && preview.load_image(path, status))
                    {
                        preview_image = path;
                        force_frame = true;
                    }
                    else if (picker == picker_type::toy_asset)
                    {
                        project.toy.passes[asset_pass].channels[asset_channel].file = path;
                        project.dirty = true;
                    }
                }
                ImGuiFileDialog::Instance()->Close();
                picker = picker_type::none;
            }
        }
    };

    std::unique_ptr<studio> app;

    void toggle_editor()
    {
        show_editor = !show_editor;
        if (app)
            app->release_keys();
    }

    void RETRO_CALLCONV keyboard(bool down, unsigned code, uint32_t character, uint16_t modifiers)
    {
        const bool repeat = code < key_down.size() && key_down[code];
        if (code < key_down.size())
            key_down[code] = down;
        if (down && !repeat && code == RETROK_F10)
            toggle_editor();

        if (ui)
        {
            ui_scope scope;
            auto &io = ImGui::GetIO();
            io.AddKeyEvent(ImGuiMod_Ctrl, (modifiers & RETROKMOD_CTRL) != 0);
            io.AddKeyEvent(ImGuiMod_Shift, (modifiers & RETROKMOD_SHIFT) != 0);
            io.AddKeyEvent(ImGuiMod_Alt, (modifiers & RETROKMOD_ALT) != 0);
            auto key = imgui_key(code);
            if (key != ImGuiKey_None)
                io.AddKeyEvent(key, down);
            if (show_editor && down && character && !(modifiers & (RETROKMOD_CTRL | RETROKMOD_ALT)))
                io.AddInputCharacter(character);
            if (app && show_editor && code != RETROK_F10)
                app->key(down, code, repeat);
        }

        if (app && !show_editor && code != RETROK_F10)
        {
            const unsigned mapped = toy_key(code);
            if (mapped)
                app->toy_runtime.key(mapped, down, repeat);
        }
    }

    void options()
    {
        if (!environment)
            return;
        retro_variable resolution{"shader_studio_resolution", nullptr};
        if (environment(RETRO_ENVIRONMENT_GET_VARIABLE, &resolution) && resolution.value)
        {
            unsigned w = 1280, h = 720;
            if (!std::strcmp(resolution.value, "960x540")) { w = 960; h = 540; }
            if (!std::strcmp(resolution.value, "1920x1080")) { w = 1920; h = 1080; }
            if (w != width || h != height)
            {
                width = w;
                height = h;
                retro_game_geometry geometry{width, height, 1920, 1080, 16.f / 9.f};
                if (loaded)
                    environment(RETRO_ENVIRONMENT_SET_GEOMETRY, &geometry);
            }
        }
        retro_variable visible{"shader_studio_editor", nullptr};
        if (environment(RETRO_ENVIRONMENT_GET_VARIABLE, &visible) && visible.value && editor_option != visible.value)
        {
            editor_option = visible.value;
            show_editor = editor_option != "disabled";
        }
    }

    std::filesystem::path contentless_root()
    {
        const char *directory = nullptr;
        if (environment)
            environment(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &directory);

        std::filesystem::path root;
        if (directory && *directory)
            root = std::filesystem::u8path(directory);
        else
        {
            directory = nullptr;
            if (environment)
                environment(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &directory);
            if (directory && *directory)
                root = std::filesystem::u8path(directory);
            else
                root = std::filesystem::current_path();
        }

        root /= "Shadertoy Studio";
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        if (ec)
            throw std::runtime_error("Cannot create the contentless Shadertoy workspace.");
        return root;
    }

    void create_contentless_project(studio_project &next, std::filesystem::path &path, std::string &error)
    {
        const auto root = contentless_root();
        const auto image = root / "Image.glsl";
        path = root / "ShaderStudio.stoy";

        if (std::filesystem::is_regular_file(path))
        {
            if (!next.load(path, error))
                throw std::runtime_error(error);
            return;
        }

        if (!std::filesystem::is_regular_file(image))
        {
            static constexpr const char *default_shader =
                "void mainImage(out vec4 fragColor, in vec2 fragCoord)\n"
                "{\n"
                "    vec2 uv = (2.0 * fragCoord - iResolution.xy) / iResolution.y;\n"
                "    float d = length(uv);\n"
                "    float a = atan(uv.y, uv.x);\n"
                "    vec3 col = 0.5 + 0.5 * cos(iTime + a + vec3(0.0, 2.0, 4.0));\n"
                "    col *= 0.25 / max(abs(d - 0.45 - 0.05 * sin(iTime * 2.0 + a * 6.0)), 0.01);\n"
                "    fragColor = vec4(col, 1.0);\n"
                "}\n";
            if (!shader_save_source(image, default_shader, error))
                throw std::runtime_error(error);
        }

        shadertoy_project raw;
        raw.passes[5].source = image;
        if (!raw.save(path, error))
            throw std::runtime_error(error);
        if (!next.load(path, error))
            throw std::runtime_error(error);
    }

    void *load_gl(const char *name)
    {
        return reinterpret_cast<void *>(hardware.get_proc_address(name));
    }

    void discard_session()
    {
        ui_scope scope;
        if (app)
        {
            // discard_session() can run after the frontend has already destroyed
            // the GL context. Do not issue GL calls from studio_preview::~studio_preview().
            app->preview.abandon_context();
            app->toy_runtime.abandon_context();
        }
        if (backend)
            studio_imgui_abandon_context();
        backend = false;
        context = false;
        app.reset();
        if (ui)
            ImGui::DestroyContext(ui);
        ui = nullptr;
    }

    void RETRO_CALLCONV context_destroy()
    {
        ui_scope scope;
        if (app)
        {
            app->release_preview_context();
            app->toy_runtime.clear();
        }
        if (backend)
            ImGui_ImplOpenGL3_Shutdown();
        backend = false;
        context = false;
        if (!loaded)
            discard_session();
    }

    void RETRO_CALLCONV context_reset()
    {
        try
        {
            ui_scope scope;
            if (context)
            {
                if (app)
                {
                    app->release_preview_context();
                    app->toy_runtime.abandon_context();
                }
                if (backend)
                    studio_imgui_abandon_context();
            }
            backend = false;
            context = false;
            if (!hardware.get_proc_address || !gladLoadGLLoader(load_gl))
                throw std::runtime_error("Cannot load OpenGL functions.");

#define LOAD_GL(name) name = reinterpret_cast<decltype(name)>(load_gl(#name)); if (!name) throw std::runtime_error("Missing " #name)
            LOAD_GL(glGenSamplers);
            LOAD_GL(glDeleteSamplers);
            LOAD_GL(glBindSampler);
            LOAD_GL(glSamplerParameteri);
#undef LOAD_GL

            bool es = std::strstr(reinterpret_cast<const char *>(glGetString(GL_VERSION)), "OpenGL ES") != nullptr;
            if (GLVersion.major < 3 || (!es && GLVersion.major == 3 && GLVersion.minor < 3))
                throw std::runtime_error("Shadertoy requires OpenGL 3.3 or GLES 3.0.");
            context = true;
            backend = ImGui_ImplOpenGL3_Init(es ? "#version 300 es" : "#version 330 core");
            if (!backend)
                throw std::runtime_error("Cannot initialize the Shadertoy editor renderer.");
            if (app)
            {
                app->restore_preview_context();
                app->compile();
            }
        }
        catch (const std::exception &e)
        {
            report(e.what());
        }
    }
}

RETRO_API unsigned retro_api_version() { return RETRO_API_VERSION; }

RETRO_API void retro_set_environment(retro_environment_t cb)
{
    environment = cb;
    static const retro_variable variables[] = {
        {"shader_studio_resolution", "Resolution; 1280x720|960x540|1920x1080"},
        {"shader_studio_editor", "Show Shadertoy editor; enabled|disabled"},
        {nullptr, nullptr}};
    if (cb)
    {
        bool support_no_game = true;
        cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &support_no_game);
        cb(RETRO_ENVIRONMENT_SET_VARIABLES, const_cast<retro_variable *>(variables));
    }
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { audio = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state = cb; }

RETRO_API void retro_init()
{
    retro_log_callback log{};
    if (environment && environment(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
        logger = log.log;
}

RETRO_API void retro_get_system_info(retro_system_info *info)
{
    *info = {"Shadertoy Studio", "1.2", "stoy|glsl|frag|json", true, true};
}

RETRO_API void retro_get_system_av_info(retro_system_av_info *info)
{
    *info = {{width, height, 1920, 1080, 16.f / 9.f}, {60, 44100}};
}

RETRO_API bool retro_load_game(const retro_game_info *game)
{
    try
    {
        if (loaded || !environment)
            return false;
        discard_session();

        std::string error;
        std::filesystem::path path;
        studio_project initial;

        if (!game || !game->path || !*game->path)
        {
            create_contentless_project(initial, path, error);
        }
        else
        {
            path = std::filesystem::absolute(std::filesystem::u8path(game->path));
            if (!initial.load(path, error))
            {
                report(error);
                return false;
            }
        }

        if (!validate_project(initial, error))
        {
            report(error);
            return false;
        }

        ui = ImGui::CreateContext();
        {
            ui_scope scope;
            auto &io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;
            io.BackendPlatformName = "shader_studio_libretro";
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            io.Fonts->AddFontDefault();
            shader_source_editor_init_font();
            ImGui::StyleColorsDark();
            auto &style = ImGui::GetStyle();
            style.WindowRounding = 5;
            style.FrameRounding = 3;

            app = std::make_unique<studio>();
            app->project = std::move(initial);
            app->initialize_paths_from_project();
            app->open_editor();
        }

        options();

        hardware = {};
        retro_hw_context_type preferred = RETRO_HW_CONTEXT_OPENGL_CORE;
        environment(RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER, &preferred);
        const bool es = preferred == RETRO_HW_CONTEXT_OPENGLES2 ||
                        preferred == RETRO_HW_CONTEXT_OPENGLES3 ||
                        preferred == RETRO_HW_CONTEXT_OPENGLES_VERSION;
        hardware.context_type = es ? RETRO_HW_CONTEXT_OPENGLES3 : RETRO_HW_CONTEXT_OPENGL_CORE;
        hardware.version_major = 3;
        hardware.version_minor = es ? 0 : 3;
        hardware.context_reset = context_reset;
        hardware.context_destroy = context_destroy;
        hardware.bottom_left_origin = true;

        retro_pixel_format format = RETRO_PIXEL_FORMAT_XRGB8888;
        if (!environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format) ||
            !environment(RETRO_ENVIRONMENT_SET_HW_RENDER, &hardware))
            throw std::runtime_error("The frontend must provide OpenGL 3.3 or GLES 3.0 hardware rendering.");
        if (!hardware.get_proc_address || !hardware.get_current_framebuffer)
            throw std::runtime_error("The frontend did not supply the required hardware rendering callbacks.");

        retro_keyboard_callback keys{keyboard};
        environment(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &keys);
        loaded = true;
        return true;
    }
    catch (const std::exception &e)
    {
        report(e.what());
        retro_unload_game();
        return false;
    }
}

RETRO_API void retro_unload_game()
{
    if (ui && app)
    {
        ui_scope scope;
        app->close_dialogs_for_unload();
    }

    loaded = false;
    reset_pending = false;
    last_l3 = false;
    last_mouse = false;
    key_down.fill(false);
    reported.clear();
    editor_option.clear();

    // GPU objects are released in context_destroy, where a current context is guaranteed.
    // If the frontend never calls it, deinit abandons invalid names without GL calls.
    if (!context)
        discard_session();
}

RETRO_API void retro_deinit()
{
    retro_unload_game();
    discard_session();
    logger = nullptr;
}

RETRO_API void retro_reset()
{
    reset_pending = true;
}

RETRO_API void retro_run()
{
    if (!loaded || !app)
        return;

    try
    {
        if (input_poll)
            input_poll();

        bool variables_changed = false;
        if (environment(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &variables_changed) && variables_changed)
            options();

        if (!context || !backend)
        {
            if (video)
                video(nullptr, width, height, 0);
            silence();
            return;
        }

        ui_scope scope;

        bool l3 = input_state_value(RETRO_DEVICE_JOYPAD, RETRO_DEVICE_ID_JOYPAD_L3) != 0;
        if (l3 && !last_l3)
            toggle_editor();
        last_l3 = l3;

        mouse_x = std::clamp(mouse_x + input_state_value(RETRO_DEVICE_MOUSE, RETRO_DEVICE_ID_MOUSE_X),
                             0.f, static_cast<float>(width));
        mouse_y = std::clamp(mouse_y + input_state_value(RETRO_DEVICE_MOUSE, RETRO_DEVICE_ID_MOUSE_Y),
                             0.f, static_cast<float>(height));

        bool pointer_pressed = input_state_value(RETRO_DEVICE_POINTER, RETRO_DEVICE_ID_POINTER_PRESSED) != 0;
        if (pointer_pressed)
        {
            mouse_x = (input_state_value(RETRO_DEVICE_POINTER, RETRO_DEVICE_ID_POINTER_X) + 32767.f) / 65534.f * width;
            mouse_y = (input_state_value(RETRO_DEVICE_POINTER, RETRO_DEVICE_ID_POINTER_Y) + 32767.f) / 65534.f * height;
        }

        bool mouse = pointer_pressed || input_state_value(RETRO_DEVICE_MOUSE, RETRO_DEVICE_ID_MOUSE_LEFT) != 0;
        auto &io = ImGui::GetIO();
        io.DisplaySize = {static_cast<float>(width), static_cast<float>(height)};
        io.DeltaTime = 1.f / 60.f;
        io.MouseDrawCursor = show_editor;
        io.AddMousePosEvent(mouse_x, mouse_y);
        io.AddMouseButtonEvent(0, mouse);
        io.AddMouseButtonEvent(1, input_state_value(RETRO_DEVICE_MOUSE, RETRO_DEVICE_ID_MOUSE_RIGHT) != 0);
        io.AddMouseWheelEvent(0, static_cast<float>(
            input_state_value(RETRO_DEVICE_MOUSE, RETRO_DEVICE_ID_MOUSE_WHEELUP) -
            input_state_value(RETRO_DEVICE_MOUSE, RETRO_DEVICE_ID_MOUSE_WHEELDOWN)));

        if (reset_pending)
        {
            app->restart();
            reset_pending = false;
        }

        audio_submitted = false;
        if (!show_editor)
            app->draw_fullscreen(width, height, mouse_x, mouse_y, mouse, mouse && !last_mouse);
        last_mouse = mouse;

        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(hardware.get_current_framebuffer()));
        glViewport(0, 0, width, height);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_RASTERIZER_DISCARD);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        if (!std::strstr(reinterpret_cast<const char *>(glGetString(GL_VERSION)), "OpenGL ES"))
            glDisable(GL_FRAMEBUFFER_SRGB);
        glClearColor(0.055f, 0.065f, 0.085f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();

        if (show_editor)
        {
            app->draw();
        }
        else if (app->toy_runtime.texture)
        {
            ImGui::GetBackgroundDrawList()->AddImage(
                static_cast<ImTextureID>(app->toy_runtime.texture),
                {0, 0}, io.DisplaySize, {0, 1}, {1, 0});
        }

        ImGui::Render();

        // app->draw() may render the Shadertoy into its own FBO, just as the
        // standalone studio does. Rebind the libretro frontend-owned target
        // before the ImGui backend emits the composed studio frame.
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(hardware.get_current_framebuffer()));
        glViewport(0, 0, width, height);
        glDisable(GL_RASTERIZER_DISCARD);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        if (!std::strstr(reinterpret_cast<const char *>(glGetString(GL_VERSION)), "OpenGL ES"))
            glDisable(GL_FRAMEBUFFER_SRGB);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (!audio_submitted)
            silence();
        if (video)
            video(RETRO_HW_FRAME_BUFFER_VALID, width, height, 0);
    }
    catch (const std::exception &e)
    {
        report(e.what());
        if (video)
            video(nullptr, width, height, 0);
        silence();
    }
}

RETRO_API void retro_set_controller_port_device(unsigned, unsigned) {}
RETRO_API unsigned retro_get_region() { return RETRO_REGION_NTSC; }
RETRO_API size_t retro_serialize_size() { return 0; }
RETRO_API bool retro_serialize(void *, size_t) { return false; }
RETRO_API bool retro_unserialize(const void *, size_t) { return false; }
RETRO_API void retro_cheat_reset() {}
RETRO_API void retro_cheat_set(unsigned, bool, const char *) {}
RETRO_API bool retro_load_game_special(unsigned, const retro_game_info *, size_t) { return false; }
RETRO_API void *retro_get_memory_data(unsigned) { return nullptr; }
RETRO_API size_t retro_get_memory_size(unsigned) { return 0; }
