#include "project.h"
#include "preview.h"
#include "workspace.h"
#include "shadertoy_runtime.h"
#include "shadertoy_download.h"
#include "shader_source_editor.h"
#include "shader_library.h"
#include "shader_gl.h"
#include "glad.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_opengl3.h"
#include "ImGuiFileDialog.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <future>
#include <chrono>
#include <initializer_list>
#include <stdexcept>

void shadertoy_smoke(const std::filesystem::path &root);

namespace
{
    void require(bool ok, const std::string &message)
    {
        if (!ok)
            throw std::runtime_error(message);
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

    struct pass_choice
    {
        const char *value, *label;
    };
    bool pass_dropdown(const char *label, std::string &value, std::initializer_list<pass_choice> choices)
    {
        const char *preview = value.empty() ? "Preset default" : value.c_str();
        for (const auto &choice : choices)
            if (value == choice.value)
                preview = choice.label;
        bool edited = false;
        if (ImGui::BeginCombo(label, preview))
        {
            if (ImGui::Selectable("Preset default", value.empty()))
            {
                edited = !value.empty();
                value.clear();
            }
            for (const auto &choice : choices)
            {
                if (ImGui::Selectable(choice.label, value == choice.value))
                {
                    edited = value != choice.value;
                    value = choice.value;
                }
                if (value == choice.value)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        return edited;
    }
    bool pass_checkbox(const char *label, std::string &value)
    {
        ImGui::PushID(label);
        // Empty is distinct from explicit false. Keep imported values untouched
        // until the user changes the option or restores its preset default.
        bool enabled = value == "true" || value == "1";
        bool edited = ImGui::Checkbox(label, &enabled);
        if (edited)
            value = enabled ? "true" : "false";
        ImGui::SameLine();
        if (value.empty())
            ImGui::TextDisabled("(preset default)");
        else
        {
            if (ImGui::SmallButton("Default"))
            {
                value.clear();
                edited = true;
            }
            if (value != "true" && value != "false" && value != "1" && value != "0" && !value.empty())
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", value.c_str());
            }
        }
        ImGui::PopID();
        return edited;
    }
    bool pass_scale_mode(const char *label, std::string &value)
    {
        return pass_dropdown(label, value, {{"source", "Source size"}, {"viewport", "Viewport size"}, {"absolute", "Absolute pixels"}});
    }

    struct graphics
    {
        SDL_Window *window = nullptr;
        SDL_GLContext context = nullptr;
        bool imgui = false, platform = false, renderer = false, gl = false;
        explicit graphics(bool hidden)
        {
            try
            {
                require(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
                SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
                window = SDL_CreateWindow("WTFweg Shader Studio", 1280, 720,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (hidden ? SDL_WINDOW_HIDDEN : 0));
                require(window != nullptr, SDL_GetError());
                context = SDL_GL_CreateContext(window);
                require(context != nullptr, SDL_GetError());
                require(gladLoadGLLoader(reinterpret_cast<GLADloadproc>(SDL_GL_GetProcAddress)) != 0,
                        "OpenGL 4.5 is required for the shader preview.");
                gl = true;
                SDL_GL_SetSwapInterval(hidden ? 0 : 1);
                ImGui::CreateContext();
                imgui = true;
                auto &io = ImGui::GetIO();
                io.IniFilename = nullptr; // Keep the host application's layout independent.
                io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
                io.Fonts->AddFontDefault();
                shader_source_editor_init_font();
                ImGui::StyleColorsDark();
                auto &style = ImGui::GetStyle();
                style.WindowRounding = 5;
                style.FrameRounding = 3;
                platform = ImGui_ImplSDL3_InitForOpenGL(window, context);
                require(platform, SDL_GetError());
                renderer = ImGui_ImplOpenGL3_Init("#version 450");
                require(renderer, "Cannot initialize the OpenGL UI renderer.");
            }
            catch (...)
            {
                close();
                throw;
            }
        }
        void close()
        {
            if (gl)
                shader_gl_destroy();
            if (renderer)
                ImGui_ImplOpenGL3_Shutdown();
            if (platform)
                ImGui_ImplSDL3_Shutdown();
            if (imgui)
                ImGui::DestroyContext();
            if (context)
                SDL_GL_DestroyContext(context);
            if (window)
                SDL_DestroyWindow(window);
            SDL_Quit();
        }
        ~graphics() { close(); }
    };

    class studio
    {
    public:
        studio_project project;
        studio_preview preview;
        shadertoy_runtime toy_runtime;
        shader_source_editor editor;
        bool done = false;
        unsigned last_fbo = 0;
        explicit studio()
        {
            auto &control = video_shaders();
            control.initialized = true; // Do not load or write WTFweg's default stack.
            control.directory = std::filesystem::u8path(SDL_GetBasePath());
            librashader().load(control.directory);
            control.source_fps = 60;
            preview.test_card();
            new_directory = (std::filesystem::current_path() / "my-shader").u8string();
            url_directory = (std::filesystem::current_path() / "shadertoy-download").u8string();
        }
        ~studio()
        {
            download_cancel = true;
            if (download.valid())
                download.wait();
        }
        void open_url(const std::string &url = {})
        {
            action([this, url]
                   { if (!url.empty()) url_input = url; url_popup = true; });
        }
        void compile()
        {
            if (project.is_shadertoy)
            {
                project.sync_toy();
                toy_runtime.compile(project.toy);
                last_toy_frame = 0;
                force_frame = true;
                return;
            }
            toy_runtime.clear();
            auto &control = video_shaders();
            control.settings = {true, {project.chain}};
            control.changed();
            if (!project.chain.passes.empty() && librashader().gl_available && control.inspect(control.settings.chains[0]))
            {
                project.chain.parameters = control.settings.chains[0].parameters;
                project.chain.values = control.settings.chains[0].values;
            }
            force_frame = true;
        }
        void open_editor()
        {
            editor.open_preset(project.chain, status);
        }
        void event(const SDL_Event &e)
        {
            if (!project.is_shadertoy)
                return;
            if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST)
            {
                toy_runtime.release_keys();
                toy_runtime.mouse(0, 0, false, false);
                preview_focused = mouse_drag = false;
            }
            if (e.type != SDL_EVENT_KEY_DOWN && e.type != SDL_EVENT_KEY_UP)
                return;
            if (e.type == SDL_EVENT_KEY_DOWN && (!preview_focused || ImGui::GetIO().WantTextInput))
                return;
            unsigned code = 0;
            auto k = e.key.key;
            if (k >= SDLK_A && k <= SDLK_Z)
                code = static_cast<unsigned>(k - SDLK_A + 65);
            else if (k >= SDLK_0 && k <= SDLK_9)
                code = static_cast<unsigned>(k);
            else if (k >= SDLK_F1 && k <= SDLK_F12)
                code = static_cast<unsigned>(k - SDLK_F1 + 112);
            else
                switch (k)
                {
                case SDLK_BACKSPACE:
                    code = 8;
                    break;
                case SDLK_TAB:
                    code = 9;
                    break;
                case SDLK_RETURN:
                    code = 13;
                    break;
                case SDLK_LSHIFT:
                case SDLK_RSHIFT:
                    code = 16;
                    break;
                case SDLK_LCTRL:
                case SDLK_RCTRL:
                    code = 17;
                    break;
                case SDLK_LALT:
                case SDLK_RALT:
                    code = 18;
                    break;
                case SDLK_ESCAPE:
                    code = 27;
                    break;
                case SDLK_SPACE:
                    code = 32;
                    break;
                case SDLK_PAGEUP:
                    code = 33;
                    break;
                case SDLK_PAGEDOWN:
                    code = 34;
                    break;
                case SDLK_END:
                    code = 35;
                    break;
                case SDLK_HOME:
                    code = 36;
                    break;
                case SDLK_LEFT:
                    code = 37;
                    break;
                case SDLK_UP:
                    code = 38;
                    break;
                case SDLK_RIGHT:
                    code = 39;
                    break;
                case SDLK_DOWN:
                    code = 40;
                    break;
                case SDLK_INSERT:
                    code = 45;
                    break;
                case SDLK_DELETE:
                    code = 46;
                    break;
                case SDLK_SEMICOLON:
                    code = 186;
                    break;
                case SDLK_EQUALS:
                    code = 187;
                    break;
                case SDLK_COMMA:
                    code = 188;
                    break;
                case SDLK_MINUS:
                    code = 189;
                    break;
                case SDLK_PERIOD:
                    code = 190;
                    break;
                case SDLK_SLASH:
                    code = 191;
                    break;
                case SDLK_GRAVE:
                    code = 192;
                    break;
                case SDLK_LEFTBRACKET:
                    code = 219;
                    break;
                case SDLK_BACKSLASH:
                    code = 220;
                    break;
                case SDLK_RIGHTBRACKET:
                    code = 221;
                    break;
                case SDLK_APOSTROPHE:
                    code = 222;
                    break;
                }
            if (code)
                toy_runtime.key(code, e.type == SDL_EVENT_KEY_DOWN, e.key.repeat);
        }
        bool create(const std::filesystem::path &path, shader_template kind)
        {
            if (!project.create(path, kind, status))
                return false;
            reset_editor();
            compile();
            open_editor();
            return true;
        }
        void request_exit()
        {
            if (download.valid())
            {
                download_cancel = true;
                exit_after_download = true;
                return;
            }
            action([this]
                   { done = true; });
        }
        void draw()
        {
            if (download.valid() && download.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            {
                auto result = download.get();
                if (result.ok && !download_cancel && !exit_after_download)
                {
                    project = std::move(result.project);
                    reset_editor();
                    compile();
                    open_editor();
                    status = "Shadertoy project downloaded. See JSON import notes for any assets that need attention.";
                    close_url_popup = true;
                }
                else
                {
                    status = result.error.empty() ? "Download cancelled." : result.error;
                    url_error = status;
                }
                if (exit_after_download)
                {
                    exit_after_download = false;
                    request_exit();
                }
            }
            auto &control = video_shaders();
            if (ImGui::BeginMainMenuBar())
            {
                if (ImGui::BeginMenu("Project"))
                {
                    if (ImGui::MenuItem("New project..."))
                        action([this]
                               { new_popup = true; });
                    if (ImGui::MenuItem("Open preset / shader..."))
                        action([this]
                               { pick(picker_type::open, "Open project or shader", ".slangp,.slang,.stoy,.glsl,.frag,.json"); });
                    if (ImGui::MenuItem("Open Shadertoy URL..."))
                        open_url();
                    if (ImGui::MenuItem("Save project", nullptr, false, !project.chain.passes.empty()))
                        save_project();
                    if (ImGui::MenuItem("Save preset as...", nullptr, false, !project.chain.passes.empty()))
                        pick(picker_type::save, "Save shader project", project.is_shadertoy ? ".stoy" : ".slangp");
                    ImGui::Separator();
                    if (ImGui::MenuItem("Exit"))
                        request_exit();
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
                if (!project.is_shadertoy && !librashader().gl_available)
                    ImGui::TextWrapped("%s\nPlace a compatible librashader runtime beside this executable, then restart to enable the preview.",
                                       librashader().status.c_str());
                ImGui::BeginDisabled(picker != picker_type::none || static_cast<bool>(pending));
                if (ImGui::Button("New project..."))
                    action([this]
                           { new_popup = true; });
                ImGui::SameLine();
                if (ImGui::Button("Open..."))
                    action([this]
                           { pick(picker_type::open, "Open project or shader", ".slangp,.slang,.stoy,.glsl,.frag,.json"); });
                if (ImGui::Button("Open Shadertoy URL..."))
                    open_url();
                if (!project.chain.passes.empty())
                {
                    if (ImGui::Button("Save project"))
                        save_project();
                    ImGui::SameLine();
                    if (ImGui::Button("Edit all passes"))
                        open_editor();
                    if (project.is_shadertoy)
                        draw_toy_passes();
                    else
                    {
                        int pass_template = template_index == 1 ? 1 : 0;
                        if (ImGui::Combo("New pass template", &pass_template, "Image effect\0Animated procedural\0"))
                            template_index = pass_template;
                        if (ImGui::Button("Add new pass") && project.add_pass(static_cast<shader_template>(pass_template), status))
                        {
                            compile();
                            open_editor();
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Add existing..."))
                            pick(picker_type::pass, "Add shader pass", ".slang");
                        draw_passes();
                        draw_parameters();
                        draw_globals();
                    }
                }
                else
                {
                    ImGui::TextWrapped("Create a Shadertoy, image-effect or animated procedural project."\ 
                        "Edit GLSL and press F5 to save and compile while the preview runs.");
                }
                ImGui::EndDisabled();
            }
            ImGui::End();
            draw_preview();
            if (editor.is_visible())
            {
                ImGui::SetNextWindowPos(workspace.editor_pos);
                if (editor.draw(project.is_shadertoy ? toy_runtime.error : control.error,
                                project.is_shadertoy ? toy_runtime.diagnostics : control.diagnostics, workspace.editor_size, "GLSL / Slang", studio_workspace::flags))
                    compile();
            }
            dialogs();
        }

    private:
        enum class picker_type
        {
            none,
            open,
            save,
            pass,
            image,
            video,
            toy_asset,
            texture
        } picker = picker_type::none;
        std::string status, new_directory, global_key, global_value;
        struct download_result
        {
            studio_project project;
            std::string error;
            bool ok = false;
        };
        std::future<download_result> download;
        std::atomic_bool download_cancel{false};
        std::string url_input, url_key, url_directory, url_error;
        bool url_popup = false, close_url_popup = false, url_assets = true, exit_after_download = false;
        int template_index = 0, output_width = 640, output_height = 480;
        bool new_popup = false, help = false, paused = false, original = false, force_frame = true;
        uint64_t next_frame = 0;
        uint64_t last_toy_frame = 0;
        bool preview_focused = false, mouse_drag = false;
        int asset_pass = 0, asset_channel = 0;
        studio_workspace workspace;
        std::function<void()> pending;
        shader_template template_kind() const { return static_cast<shader_template>(template_index); }
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
                pick(picker_type::save, "Save shader project", project.is_shadertoy ? ".stoy" : ".slangp");
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
                            if ((file_input && c.kind != toy_input_kind::audio) || c.kind == toy_input_kind::camera)
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
            ImGui::TextWrapped("Uniforms: iResolution, iTime, iTimeDelta, iFrame, iFrameRate, iMouse,"\ 
                "iDate, iSampleRate, iChannel0..3, iChannelTime[4], iChannelResolution[4].");
        }
        void draw_passes()
        {
            int remove = -1, move = -1, direction = 0;
            for (int i = 0; i < static_cast<int>(project.chain.passes.size()); ++i)
            {
                auto &pass = project.chain.passes[i];
                ImGui::PushID(i);
                ImGui::Separator();
                if (ImGui::Checkbox("##enabled", &pass.enabled))
                    changed();
                ImGui::SameLine();
                ImGui::Text("%d. %s", i + 1, pass.source.filename().u8string().c_str());
                if (ImGui::SmallButton("Edit"))
                {
                    editor.open(pass.source, status);
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(i == 0);
                if (ImGui::SmallButton("Up"))
                {
                    move = i;
                    direction = -1;
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(i + 1 == static_cast<int>(project.chain.passes.size()));
                if (ImGui::SmallButton("Down"))
                {
                    move = i;
                    direction = 1;
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(project.chain.passes.size() == 1);
                if (ImGui::SmallButton("Remove"))
                    remove = i;
                ImGui::EndDisabled();
                if (ImGui::TreeNode("Pass options"))
                {
                    bool edited = pass_dropdown("Sampling", pass.options["filter_linear"], {{"false", "Nearest"}, {"true", "Linear"}});
                    edited |= pass_dropdown("Wrap mode", pass.options["wrap_mode"],
                                            {{"clamp_to_edge", "Clamp to edge"}, {"clamp_to_border", "Clamp to border"}, 
                                            {"repeat", "Repeat"}, {"mirrored_repeat", "Mirrored repeat"}});
                    edited |= pass_checkbox("Input mipmaps", pass.options["mipmap_input"]);
                    edited |= pass_checkbox("Float framebuffer", pass.options["float_framebuffer"]);
                    edited |= pass_checkbox("sRGB framebuffer", pass.options["srgb_framebuffer"]);
                    edited |= pass_scale_mode("Scale mode", pass.options["scale_type"]);
                    edited |= input("Scale", pass.options["scale"]);
                    if (ImGui::TreeNode("Per-axis scaling"))
                    {
                        edited |= pass_scale_mode("X scale mode", pass.options["scale_type_x"]);
                        edited |= input("X scale", pass.options["scale_x"]);
                        edited |= pass_scale_mode("Y scale mode", pass.options["scale_type_y"]);
                        edited |= input("Y scale", pass.options["scale_y"]);
                        ImGui::TreePop();
                    }
                    edited |= input("Pass alias", pass.options["alias"]);
                    edited |= input("Frame count modulus", pass.options["frame_count_mod"]);
                    project.dirty |= edited;
                    if (ImGui::Button("Apply pass options"))
                        compile();
                    ImGui::TextWrapped("Apply pass options updates the preview."\ 
                        "Save project persists the changes."\ 
                        "Scale is a multiplier for source/viewport modes, or a pixel count for absolute mode."\ 
                        "Default or an empty field leaves the option unspecified.");
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            if (remove >= 0)
            {
                project.chain.passes.erase(project.chain.passes.begin() + remove);
                changed();
                open_editor();
            }
            else if (move >= 0)
            {
                std::swap(project.chain.passes[move], project.chain.passes[move + direction]);
                changed();
                open_editor();
            }
        }
        void draw_parameters()
        {
            if (ImGui::CollapsingHeader("Parameters", ImGuiTreeNodeFlags_DefaultOpen))
            {
                for (const auto &parameter : project.chain.parameters)
                {
                    ImGui::PushID(parameter.id.c_str());
                    if (ImGui::SliderFloat(parameter.label.c_str(), &project.chain.values[parameter.id], parameter.minimum, parameter.maximum))
                    {
                        project.dirty = true;
                        force_frame = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Reset"))
                    {
                        project.chain.values[parameter.id] = parameter.initial;
                        project.dirty = true;
                        force_frame = true;
                    }
                    ImGui::PopID();
                }
            }
        }
        void draw_globals()
        {
            if (!ImGui::CollapsingHeader("Textures and preset options"))
                return;
            if (ImGui::Button("Add texture..."))
                pick(picker_type::texture, "Add preset texture", ".png,.jpg,.jpeg,.bmp,.tga");
            for (auto &entry : project.chain.globals)
                if (input(entry.first.c_str(), entry.second))
                    project.dirty = true;
            input("New option", global_key);
            input("Value", global_value);
            if (ImGui::Button("Set option") && !global_key.empty())
            {
                project.chain.globals[global_key] = global_value;
                project.dirty = true;
            }
            if (ImGui::Button("Apply preset options"))
                compile();
        }
        void draw_preview()
        {
            auto &control = video_shaders();
            workspace.prepare_preview();
            if (ImGui::Begin("Live preview", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
            {
                workspace.capture_preview();
                workspace.begin_controls();
                if (ImGui::Button("Load image..."))
                    pick(picker_type::image, "Load preview image", ".png,.jpg,.jpeg,.bmp,.tga");
                studio_workspace::next_control("Load video...", false);
                if (ImGui::Button("Load video..."))
                    pick(picker_type::video, "Load looping preview video", ".mp4,.m4v,.mov,.webm,.mkv");
                studio_workspace::next_control("Test card", false);
                if (ImGui::Button("Test card"))
                {
                    preview.test_card();
                    force_frame = true;
                }
                studio_workspace::next_control("Pause", true);
                if (ImGui::Checkbox("Pause", &paused))
                    last_toy_frame = 0;
                studio_workspace::next_control("Step", false);
                if (ImGui::Button("Step"))
                {
                    paused = true;
                    force_frame = true;
                }
                studio_workspace::next_control("Restart", false);
                if (ImGui::Button("Restart"))
                {
                    shader_gl_destroy();
                    toy_runtime.reset();
                    preview.restart_video();
                    last_toy_frame = 0;
                    preview.output_texture = 0;
                    force_frame = true;
                }
                studio_workspace::next_control("Original", true);
                ImGui::Checkbox("Original", &original);
                if (project.is_shadertoy)
                {
                    if (ImGui::Checkbox("Play audio", &toy_runtime.sound_enabled))
                        toy_runtime.media_status.clear();
                    studio_workspace::next_control("Microphone / camera", true);
                    if (ImGui::Checkbox("Microphone / camera", &toy_runtime.capture_enabled))
                        toy_runtime.media_status.clear();
                    toy_runtime.set_playing(!paused);
                    ImGui::TextWrapped("Time %.3f s | Frame %d | Click preview for keyboard input", toy_runtime.time(), toy_runtime.frame());
                }
                ImGui::Text("Resolution %d x %d", output_width, output_height);
                if (!toy_runtime.media_status.empty())
                    ImGui::TextWrapped("%s", toy_runtime.media_status.c_str());
                if (!(project.is_shadertoy ? toy_runtime.error : control.error).empty())
                    ImGui::TextWrapped("Compilation failed: the previous working preview is retained."\ 
                        "See the source editor for diagnostics.");
                workspace.end_controls();
                const auto available = ImGui::GetContentRegionAvail();
                workspace.measure_preview(ImGui::GetWindowSize(), available);
                const int new_width = std::clamp(static_cast<int>(available.x), 16, 4096);
                const int new_height = std::clamp(static_cast<int>(available.y), 16, 4096);
                force_frame |= new_width != output_width || new_height != output_height;
                output_width = new_width;
                output_height = new_height;
                const auto now = SDL_GetTicksNS();
                if (force_frame || (!paused && now >= next_frame))
                {
                    if (!control.settings.chains.empty())
                        control.settings.chains[0].values = project.chain.values;
                    control.source_aspect = static_cast<float>(preview.width) / preview.height;
                    float delta = paused || !last_toy_frame ? 1.f / 60 :
                        std::clamp(static_cast<float>(now - last_toy_frame) / 1e9f, 0.f, 0.25f);
                    preview.advance_video(delta, status);
                    if (project.is_shadertoy)
                    {
                        last_fbo = toy_runtime.render(output_width, output_height, delta);
                        preview.output_texture = toy_runtime.texture;
                    }
                    else
                        last_fbo = preview.render(output_width, output_height);
                    force_frame = false;
                    last_toy_frame = now;
                    next_frame = now + 1000000000 / 60;
                }
                const float aspect = original ? static_cast<float>(preview.width) / preview.height : 
                static_cast<float>(output_width) / output_height;
                float w = std::max(1.f, std::min(available.x, available.y * aspect));
                auto texture = original || !preview.output_texture ? preview.input_texture : preview.output_texture;
                auto top_left = ImGui::GetCursorScreenPos();
                bool flip = project.is_shadertoy && !original && preview.output_texture;
                ImGui::Image(static_cast<ImTextureID>(texture), ImVec2(w, w / aspect), flip ? ImVec2(0, 1) : 
                ImVec2(0, 0), flip ? ImVec2(1, 0) : ImVec2(1, 1));
                if (project.is_shadertoy)
                {
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
                                      std::clamp(1 - (pos.y - top_left.y) / (w / aspect), 0.f, 1.f) * output_height, down, clicked);
                    if (!down)
                        mouse_drag = false;

                }
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
                ImGui::TextWrapped("An API key uses the Public+API endpoint.\n"\
                    "The download creates an editable project in a new folder."\ 
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
                            try { result.ok = studio_download_shadertoy(url, key, std::filesystem::u8path(directory), assets,
                                result.project, download_cancel, result.error); }
                            catch (const std::exception &e) { result.error = e.what(); }
                            return result; });
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
                ImGui::Combo("Starter shader", &template_index, "Image effect\0Animated procedural\0Shadertoy\0");
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
                    if (picker == picker_type::open && project.load(path, status))
                    {
                        reset_editor();
                        compile();
                        open_editor();
                    }
                    else if (picker == picker_type::save && editor.save_all(status) && project.save(path, status))
                    {
                        compile();
                        status = "Project saved.";
                    }
                    else if (picker == picker_type::pass && project.chain.passes.size() < 64)
                    {
                        project.chain.passes.push_back({path, true, {}});
                        changed();
                        open_editor();
                    }
                    else if (picker == picker_type::video && preview.load_video(path, status))
                    {
                        last_toy_frame = 0;
                        force_frame = true;
                    }
                    else if (picker == picker_type::image && preview.load_image(path, status))
                        force_frame = true;
                    else if (picker == picker_type::toy_asset)
                    {
                        project.toy.passes[asset_pass].channels[asset_channel].file = path;
                        project.dirty = true;
                    }
                    else if (picker == picker_type::texture)
                    {
                        unsigned n = 1;
                        while (project.chain.globals.count("LUT" + std::to_string(n)))
                            ++n;
                        auto id = "LUT" + std::to_string(n);
                        auto &textures = project.chain.globals["textures"];
                        if (!textures.empty())
                            textures += ';';
                        textures += id;
                        project.chain.globals[id] = path.generic_u8string();
                        changed();
                        status = "Texture added as " + id + ". Declare a sampler with that name in your shader.";
                    }
                }
                ImGuiFileDialog::Instance()->Close();
                picker = picker_type::none;
            }
        }
    };

    void smoke_step(studio &app, int frame, const std::filesystem::path &root)
    {
        auto &control = video_shaders();
        std::string error;
        if (frame == 0)
        {
            require(librashader().gl_available, librashader().status);
            require(app.preview.render(640, 480) == app.preview.input_fbo, "Empty project did not show its input image");
            require(app.create(root / ("project-" + std::to_string(SDL_GetPerformanceCounter())),
                               shader_template::image),
                    "Cannot create smoke project");
            auto image_path = app.project.preset.parent_path() / "reference.tga";
            const unsigned char tga[] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0, 24, 32,
                                         120, 80, 40, 120, 80, 40, 120, 80, 40, 120, 80, 40};
            std::ofstream image(image_path, std::ios::binary);
            image.write(reinterpret_cast<const char *>(tga), sizeof(tga));
            image.close();
            require(app.preview.load_image(image_path, error), error);
            require(app.preview.width == 2 && app.preview.height == 2, "Loaded image dimensions are incorrect");
            unsigned char rgba[4];
            glBindFramebuffer(GL_READ_FRAMEBUFFER, app.preview.input_fbo);
            glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            require(rgba[0] == 40 && rgba[1] == 80 && rgba[2] == 120, "Loaded image pixels are incorrect");
            auto previous = app.preview.input_texture;
            require(!app.preview.load_image(image_path.parent_path() / "missing.png", error) && app.preview.input_texture == previous,
                    "Failed image loading lost the preview image");
            app.preview.test_card();
        }
        auto pixel = [&]
        {
            unsigned char rgba[4];
            glBindFramebuffer(GL_READ_FRAMEBUFFER, app.last_fbo);
            glReadPixels(20, 20, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            require(glGetError() == GL_NO_ERROR, "Preview generated an OpenGL error");
            return static_cast<int>(rgba[0]);
        };
        if (frame == 2)
        {
            require(control.error.empty(), control.error);
            require(std::abs(pixel() - 230) < 3, "Starter preview pixel mismatch");
            app.project.chain.values["GAIN"] = 0.5f;
        }
        if (frame == 4)
        {
            require(std::abs(pixel() - 115) < 3, "Live preview parameter did not update");
            auto broken = studio_shader_template(shader_template::image);
            auto at = broken.find("texture(Source, uv)");
            broken.replace(at, 7, "missing_function");
            require(shader_save_source(app.project.chain.passes[0].source, broken, error), error);
            app.compile();
        }
        if (frame == 6)
        {
            require(!control.error.empty() && !control.diagnostics.entries.empty(), "Syntax error did not produce diagnostics");
            require(std::abs(pixel() - 115) < 3, "Failed compilation lost the previous preview");
            require(shader_save_source(app.project.chain.passes[0].source, studio_shader_template(shader_template::image), error), error);
            app.compile();
        }
        if (frame == 8)
        {
            require(control.error.empty() && control.diagnostics.entries.empty(), "Corrected source retained compiler errors");
            require(app.project.add_pass(shader_template::procedural, error), error);
            app.compile();
            app.open_editor();
        }
        if (frame == 10)
        {
            require(control.error.empty(), control.error);
            require(app.project.save(app.project.preset, error), error);
            studio_project reopened;
            require(reopened.load(app.project.preset, error), error);
            require(reopened.chain.passes.size() == 2, "Saved preset lost a pass");
            app.done = true;
            std::puts("PASS: standalone studio, new project, templates, image loading,"\ 
                "live preview pixels/parameters, compiler diagnostics, rollback and preset save/reopen");
        }
    }
}

int main(int argc, char **argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--help") == 0)
    {
        std::puts("WTFweg Shader Studio\nUsage: wtfweg-shader-studio [SHADERTOY_URL | shadertoy.json"\ 
            "| project.stoy | shader.glsl | preset.slangp | shader.slang]\n"\       
            "URLs open the download dialog; JSON imports create a new sibling project folder.\n"\       
            "wtfweg-shader-studio --smoke-test OUTPUT_DIRECTORY\n"\       
            "wtfweg-shader-studio --shadertoy-smoke-test OUTPUT_DIRECTORY");
        return 0;
    }
    const bool smoke = argc == 3 && std::strcmp(argv[1], "--smoke-test") == 0;
    const bool toy_smoke = argc == 3 && std::strcmp(argv[1], "--shadertoy-smoke-test") == 0;
    if (argc > 2 && !smoke && !toy_smoke)
    {
        std::fputs("Use --help for usage.\n", stderr);
        return 2;
    }
    try
    {
        if (toy_smoke)
            SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
        graphics gpu(smoke || toy_smoke);
        if (toy_smoke)
            shadertoy_smoke(std::filesystem::absolute(std::filesystem::u8path(argv[2])));
        {
            studio app;
            if (toy_smoke)
                require(app.create(std::filesystem::absolute(std::filesystem::u8path(argv[2])) /
                                       ("ui-" + std::to_string(SDL_GetPerformanceCounter())),
                                   shader_template::shadertoy),
                        "Cannot create Shadertoy UI project");
            if (!smoke && argc == 2)
            {
                std::string error;
                std::string id;
                if (studio_shadertoy_id(argv[1], id, error))
                    app.open_url(argv[1]);
                else
                {
                    require(app.project.load(std::filesystem::u8path(argv[1]), error), error);
                    app.compile();
                    app.open_editor();
                }
            }
            int frame = 0;
            while (!app.done)
            {
                SDL_Event event;
                while (SDL_PollEvent(&event))
                {
                    ImGui_ImplSDL3_ProcessEvent(&event);
                    app.event(event);
                    if (event.type == SDL_EVENT_QUIT || (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(gpu.window)))
                        app.request_exit();
                }
                if (smoke)
                    smoke_step(app, frame, std::filesystem::u8path(argv[2]));
                ImGui_ImplOpenGL3_NewFrame();
                ImGui_ImplSDL3_NewFrame();
                ImGui::NewFrame();
                app.draw();
                if (toy_smoke && frame == 5)
                {
                    require(app.toy_runtime.error.empty(), app.toy_runtime.error);
                    require(app.preview.output_texture != 0, "Shadertoy UI preview is empty");
                    app.done = true;
                }
                ImGui::Render();
                int w, h;
                SDL_GetWindowSizeInPixels(gpu.window, &w, &h);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glViewport(0, 0, w, h);
                glClearColor(0.055f, 0.065f, 0.085f, 1);
                glClear(GL_COLOR_BUFFER_BIT);
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                SDL_GL_SwapWindow(gpu.window);
                if (smoke)
                    SDL_Delay(20);
                else
                    SDL_Delay(1);
                ++frame;
            }
            shader_gl_destroy();
        }
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "Shader Studio: %s\n", error.what());
        return 1;
    }
}
