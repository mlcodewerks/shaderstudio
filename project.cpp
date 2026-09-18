#include "project.h"
#include <stdexcept>
#include <algorithm>
#include <cctype>

std::string studio_shader_template(shader_template kind)
{
    std::string source = R"(#version 450
// WTFweg Shader Studio: edit this file, then Save source and reload (Ctrl+S).
// Source is the previous pass; Original is the original preview image.
#pragma parameter GAIN "Brightness" 1.0 0.0 2.0 0.01
layout(std140, set = 0, binding = 0) uniform UBO {
    mat4 MVP;                    // Vertex transform.
    // Size vectors contain (width, height, 1/width, 1/height).
    vec4 OriginalSize;           // Input to this preset's first pass.
    vec4 SourceSize;             // Input to the current pass.
    vec4 OutputSize;             // Current pass render target.
    vec4 FinalViewportSize;      // Final output viewport.
    uint FrameCount;             // Frame index, subject to frame_count_mod.
    int FrameDirection;          // 1 forward, -1 when rewinding.
    uint Rotation;               // Quarter turns: 0, 1, 2, or 3.
    uint TotalSubFrames;         // Subframes per frame (normally 1).
    uint CurrentSubFrame;        // Current subframe, starting at 1.
    float OriginalAspect;        // Intended source width / height.
    float OriginalAspectRotated; // Intended aspect after rotation.
    float OriginalFPS;           // Source frames per second.
    uint FrameTimeDelta;         // Milliseconds since the last frame in librashader.
    float GAIN;                 // Brightness parameter declared above.
} global;

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 uv;
void main() {
    gl_Position = global.MVP * Position;
    uv = TexCoord;
}

#pragma stage fragment
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;
void main() {
)";
    if (kind == shader_template::procedural)
        source += R"(    float time = float(global.FrameCount) / 60.0;
    vec3 color = 0.5 + 0.5 * cos(time + uv.xyx * 6.28318 + vec3(0.0, 2.0, 4.0));
)";
    else
        source += "    vec3 color = texture(Source, uv).rgb;\n";
    source += "    FragColor = vec4(color * global.GAIN, 1.0);\n}\n";
    return source;
}

bool studio_project::create(const std::filesystem::path &directory, shader_template kind, std::string &error)
try
{
    auto target = std::filesystem::absolute(directory).lexically_normal();
    if (std::filesystem::exists(target))
        throw std::runtime_error("Choose a new project folder; that path already exists.");
    std::filesystem::create_directories(target);
    studio_project next;
    next.preset = target / "project.slangp";
    next.chain.name = target.filename().u8string();
    if (kind == shader_template::shadertoy)
    {
        next.is_shadertoy = true;
        next.preset = target / "project.stoy";
        next.toy.common = target / "Common.glsl";
        if (!shader_save_source(next.toy.common, "// Shared GLSL functions and constants for every pass.\n", error) ||
            !next.toy.add(target, toy_pass_kind::image, error))
            return false;
        next.sync_toy();
        if (!next.save(next.preset, error))
            return false;
        *this = std::move(next);
        return true;
    }
    if (!next.add_pass(kind, error) || !next.save(next.preset, error))
        return false;
    *this = std::move(next);
    return true;
}
catch (const std::exception &e)
{
    error = e.what();
    return false;
}

bool studio_project::load(const std::filesystem::path &file, std::string &error)
try
{
    studio_project next;
    auto path = std::filesystem::absolute(file);
    auto extension = path.extension().u8string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c)
                   { return std::tolower(c); });
    if (extension == ".json")
    {
        if (!next.toy.import_json(path, next.preset, error))
            return false;
        next.is_shadertoy = true;
        next.chain.name = next.toy.title;
        next.sync_toy();
        *this = std::move(next);
        error.clear();
        return true;
    }
    if (path.extension() == ".stoy" || path.extension() == ".glsl" || path.extension() == ".frag")
    {
        next.is_shadertoy = true;
        if (path.extension() == ".stoy")
        {
            if (!next.toy.load(path, error))
                return false;
            next.preset = path;
        }
        else
        {
            if (!std::filesystem::is_regular_file(path))
                throw std::runtime_error("Shader source does not exist.");
            next.toy.passes[5].source = path;
            next.dirty = true;
        }
        next.chain.name = next.toy.title.empty() ? path.stem().u8string() : next.toy.title;
        next.sync_toy();
        *this = std::move(next);
        error.clear();
        return true;
    }
    if (!shader_import(path, next.chain, error))
        return false;
    next.preset = path;
    if (path.extension() == ".slang")
    {
        next.preset.clear();
        next.dirty = true;
    }
    *this = std::move(next);
    return true;
}
catch (const std::exception &e)
{
    error = e.what();
    return false;
}

bool studio_project::add_pass(shader_template kind, std::string &error)
try
{
    if (is_shadertoy || kind == shader_template::shadertoy)
    {
        error = "Use the Shadertoy pass controls to add named passes.";
        return false;
    }
    if (chain.passes.size() >= 64)
        throw std::runtime_error("A preset supports at most 64 passes.");
    auto directory = preset.empty() ? chain.passes.at(0).source.parent_path() : preset.parent_path();
    std::filesystem::path source;
    for (unsigned i = 1; i < 10000; ++i)
    {
        source = directory / ("pass-" + std::to_string(i) + ".slang");
        if (!std::filesystem::exists(source))
            break;
        source.clear();
    }
    if (source.empty())
        throw std::runtime_error("Cannot find an unused shader filename.");
    if (!shader_save_source(source, studio_shader_template(kind), error))
        return false;
    chain.passes.push_back({source, true, {{"filter_linear", "true"}, {"scale_type", "viewport"}, {"scale", "1.0"}}});
    dirty = true;
    error.clear();
    return true;
}
catch (const std::exception &e)
{
    error = e.what();
    return false;
}

bool studio_project::save(const std::filesystem::path &file, std::string &error)
try
{
    if (is_shadertoy)
    {
        auto path = std::filesystem::absolute(file);
        if (path.extension().empty())
            path += ".stoy";
        if (path.extension() != ".stoy")
        {
            error = "Shadertoy projects must use .stoy.";
            return false;
        }
        if (!toy.save(path, error))
            return false;
        preset = path;
        dirty = false;
        error.clear();
        return true;
    }
    // The portable .slangp format has no pass bypass field. Require an explicit
    // decision rather than silently dropping a disabled pass from the project.
    for (const auto &pass : chain.passes)
        if (!pass.enabled)
        {
            error = "Enable or remove bypassed passes before saving the preset.";
            return false;
        }
    auto path = std::filesystem::absolute(file);
    if (path.extension().empty())
        path += ".slangp";
    if (path.extension() != ".slangp")
    {
        error = "Presets must use the .slangp extension.";
        return false;
    }
    if (!shader_export(path, chain, error))
        return false;
    preset = path;
    dirty = false;
    return true;
}
catch (const std::exception &e)
{
    error = e.what();
    return false;
}
