#include "shadertoy_project.h"
#include <fstream>
#include <iomanip>
#include <sstream>
#include <cmath>
#include <stdexcept>

const char *toy_pass_name(toy_pass_kind kind)
{
    static const char *names[] = {"Buffer A", "Buffer B", "Buffer C", "Buffer D", "Cubemap", "Image", "Sound"};
    return names[static_cast<int>(kind)];
}
const char *toy_input_name(toy_input_kind kind)
{
    static const char *names[] = {"None", "Buffer A", "Buffer B", "Buffer C", "Buffer D", "Cubemap",
                                  "Texture", "Cubemap texture (6-face strip)", "Volume (slice atlas)", "Keyboard", "Audio",
                                  "Microphone", "Video / image sequence", "Camera"};
    return names[static_cast<int>(kind)];
}
std::string toy_template(toy_pass_kind kind)
{
    if (kind == toy_pass_kind::sound)
        return R"(// Stereo samples in [-1, 1]. iSampleRate is 44100 Hz.
vec2 mainSound(int sampleIndex, float time) {
    return vec2(0.15 * sin(6.2831853 * 440.0 * time));
}
)";
    if (kind == toy_pass_kind::cubemap)
        return R"(void mainCubemap(out vec4 fragColor, in vec2 fragCoord, in vec3 rayOrigin, in vec3 rayDirection) {
    fragColor = vec4(0.5 + 0.5 * rayDirection, 1.0);
}
)";
    return R"(// Paste Shadertoy GLSL here; uniforms and main() are supplied by Studio.
// Common is shared by all passes. Configure iChannel0..3 in the project panel.
void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = fragCoord / iResolution.xy;
    vec3 color = 0.5 + 0.5 * cos(iTime + uv.xyx + vec3(0, 2, 4));
    fragColor = vec4(color, 1.0);
}
)";
}
shader_chain_config shadertoy_project::editor_chain(const std::string &name) const
{
    shader_chain_config chain;
    chain.name = name;
    if (!common.empty())
        chain.passes.push_back({common, true, {}});
    for (const auto &p : passes)
        if (!p.source.empty())
            chain.passes.push_back({p.source, true, {}});
    return chain;
}
bool shadertoy_project::validate(std::string &error) const
{
    if (passes[5].source.empty())
    {
        error = "A Shadertoy project needs an Image pass.";
        return false;
    }
    for (const auto &p : passes)
        for (const auto &c : p.channels)
        {
            int k = static_cast<int>(c.kind);
            if (k < 0 || k > 13 || c.filter < 0 || c.filter > 2 || c.wrap < 0 || c.wrap > 1 ||
                c.depth < 1 || c.depth > 256 || !std::isfinite(c.fps) || c.fps <= 0 || c.fps > 240)
            {
                error = "Invalid Shadertoy channel settings.";
                return false;
            }
            if (!p.source.empty() && k >= 1 && k <= 5 && passes[k - 1].source.empty())
            {
                error = std::string("Channel references missing ") + toy_pass_name(static_cast<toy_pass_kind>(k - 1));
                return false;
            }
        }
    error.clear();
    return true;
}
bool shadertoy_project::add(const std::filesystem::path &dir, toy_pass_kind kind, std::string &error)
{
    auto &p = passes.at(static_cast<size_t>(kind));
    if (!p.source.empty())
    {
        error = "That pass already exists.";
        return false;
    }
    for (int n = 0; n < 10000; ++n)
    {
        auto file = dir / (std::string(toy_pass_name(kind)) + (n ? "-" + std::to_string(n) : "") + ".glsl");
        if (std::filesystem::exists(file))
            continue;
        if (!shader_save_source(file, toy_template(kind), error))
            return false;
        p.source = file;
        return true;
    }
    error = "Cannot find an unused shader filename.";
    return false;
}
bool shadertoy_project::save(const std::filesystem::path &path, std::string &error) const
try
{
    if (!validate(error))
        return false;
    auto relative = [&](const std::filesystem::path &p)
    {
        if (p.empty())
            return std::string{};
        auto rel = p.lexically_relative(path.parent_path());
        return (rel.empty() ? p : rel).generic_u8string();
    };
    std::ostringstream out;
    out << "SHADERTOY_STUDIO 1\ncommon " << std::quoted(relative(common)) << "\nvr " << vr << '\n';
    if (!title.empty())
        out << "title " << std::quoted(title) << '\n';
    for (const auto &note : import_notes)
        out << "note " << std::quoted(note) << '\n';
    for (size_t i = 0; i < passes.size(); ++i)
    {
        const auto &p = passes[i];
        out << "pass " << i << ' ' << std::quoted(relative(p.source)) << '\n';
        for (size_t j = 0; j < 4; ++j)
        {
            const auto &c = p.channels[j];
            out << "channel " << i << ' ' << j << ' ' << static_cast<int>(c.kind) << ' '
                << std::quoted(relative(c.file)) << ' ' << c.filter << ' ' << c.wrap << ' '
                << c.vflip << ' ' << c.srgb << ' ' << c.depth << ' ' << c.fps << '\n';
            if (!c.origin.empty())
                out << "origin " << i << ' ' << j << ' ' << std::quoted(c.origin) << '\n';
        }
    }
    return shader_save_source(path, out.str(), error);
}
catch (const std::exception &e)
{
    error = e.what();
    return false;
}
bool shadertoy_project::load(const std::filesystem::path &path, std::string &error)
try
{
    if (std::filesystem::file_size(path) > 1024 * 1024)
        throw std::runtime_error("Shadertoy project exceeds 1 MiB.");
    std::ifstream in(path);
    std::string magic;
    int version = 0;
    if (!(in >> magic >> version) || magic != "SHADERTOY_STUDIO" || version != 1)
        throw std::runtime_error("Unsupported Shadertoy Studio project format.");
    shadertoy_project next;
    std::string key, value;
    auto resolve = [&](const std::string &v)
    { return v.empty() ? std::filesystem::path{} : (path.parent_path() / std::filesystem::u8path(v)).lexically_normal(); };
    while (in >> key)
    {
        size_t i = 0, j = 0;
        if (key == "common")
        {
            in >> std::quoted(value);
            next.common = resolve(value);
        }
        else if (key == "vr")
            in >> next.vr;
        else if (key == "title")
            in >> std::quoted(next.title);
        else if (key == "note")
        {
            in >> std::quoted(value);
            next.import_notes.push_back(value);
        }
        else if (key == "origin")
        {
            in >> i >> j >> std::quoted(value);
            next.passes.at(i).channels.at(j).origin = value;
        }
        else if (key == "pass")
        {
            in >> i >> std::quoted(value);
            next.passes.at(i).source = resolve(value);
        }
        else if (key == "channel")
        {
            int kind = 0;
            in >> i >> j >> kind >> std::quoted(value);
            auto &c = next.passes.at(i).channels.at(j);
            c.kind = static_cast<toy_input_kind>(kind);
            c.file = resolve(value);
            in >> c.filter >> c.wrap >> c.vflip >> c.srgb >> c.depth >> c.fps;
        }
        else
            throw std::runtime_error("Unknown Shadertoy project field: " + key);
        if (!in)
            throw std::runtime_error("Malformed Shadertoy project field: " + key);
    }
    if (!next.validate(error))
        return false;
    for (const auto &p : next.editor_chain("").passes)
        if (!std::filesystem::is_regular_file(p.source))
            throw std::runtime_error("Missing shader source: " + p.source.u8string());
    *this = std::move(next);
    error.clear();
    return true;
}
catch (const std::exception &e)
{
    error = e.what();
    return false;
}
