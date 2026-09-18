#include "shadertoy_project.h"
#include "cJSON.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>

namespace
{
    using json = const cJSON *;
    using path = std::filesystem::path;
    void require(bool ok, const std::string &message)
    {
        if (!ok)
            throw std::runtime_error(message);
    }
    json field(json object, const char *name) { return cJSON_GetObjectItemCaseSensitive(object, name); }
    std::string string(json value, const std::string &label)
    {
        require(cJSON_IsString(value), label + " must be a string.");
        return value->valuestring;
    }
    std::string optional_string(json object, const char *key, const std::string &fallback = {})
    {
        auto value = field(object, key);
        return value ? string(value, key) : fallback;
    }
    int integer(json value, int low, int high, const std::string &label)
    {
        require(cJSON_IsNumber(value) && std::isfinite(value->valuedouble) &&
                    value->valuedouble >= low && value->valuedouble <= high && std::floor(value->valuedouble) == value->valuedouble,
                label + " must be an integer from " + std::to_string(low) + " to " + std::to_string(high) + '.');
        return static_cast<int>(value->valuedouble);
    }
    std::string identifier(json value)
    {
        if (!value)
            return {};
        if (cJSON_IsString(value))
        {
            auto result = string(value, "Asset ID");
            require(!result.empty(), "Asset ID must not be empty.");
            return result;
        }
        require(cJSON_IsNumber(value) && std::isfinite(value->valuedouble) && value->valuedouble >= 0 &&
                    value->valuedouble <= 9007199254740991.0 && std::floor(value->valuedouble) == value->valuedouble,
                "Asset ID must be a string or a non-negative integer.");
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::fixed << std::setprecision(0) << value->valuedouble;
        return out.str();
    }
    bool flag(json object, const char *key, bool fallback)
    {
        auto value = field(object, key);
        if (!value)
            return fallback;
        if (cJSON_IsBool(value))
            return cJSON_IsTrue(value);
        if (cJSON_IsString(value))
        {
            auto text = string(value, key);
            if (text == "true" || text == "1")
                return true;
            if (text == "false" || text == "0")
                return false;
        }
        if (cJSON_IsNumber(value) && (value->valuedouble == 0 || value->valuedouble == 1))
            return value->valuedouble == 1;
        throw std::runtime_error(std::string(key) + " must be true or false.");
    }
    std::string read(const path &file, size_t limit)
    {
        auto size = std::filesystem::file_size(file);
        require(size <= limit, "File exceeds import size limit: " + file.u8string());
        std::ifstream in(file, std::ios::binary);
        std::string text(static_cast<size_t>(size), '\0');
        require(static_cast<bool>(in.read(text.data(), size)), "Cannot read " + file.u8string());
        require(text.find('\0') == text.npos, "Import contains NUL bytes.");
        return text;
    }
    void validate_json(json node, int depth = 0)
    {
        require(depth <= 64, "JSON nesting exceeds 64 levels.");
        std::set<std::string> names;
        for (json child = node->child; child; child = child->next)
        {
            if (cJSON_IsObject(node))
                require(child->string && names.insert(child->string).second, "Duplicate JSON object field.");
            validate_json(child, depth + 1);
        }
    }
    struct imported_pass
    {
        json object;
        int slot; // 7 is Common, -1 is an unnamed buffer awaiting assignment.
        std::string code, output;
    };
    int buffer_slot(const std::string &name)
    {
        for (int i = 0; i < 4; ++i)
            if (name == std::string("Buffer ") + char('A' + i) || name == std::string("Buf ") + char('A' + i))
                return i;
        return -1;
    }
    path asset_path(const std::string &reference, const path &directory)
    {
        if (reference.empty())
            return {};
        std::string local = reference;
        auto scheme = local.find("://");
        if (scheme != local.npos)
        {
            auto slash = local.find('/', scheme + 3);
            if (slash == local.npos)
                return {};
            local.erase(0, slash);
        }
        // Website asset paths are rooted at /media, not the local filesystem root.
        if (scheme != reference.npos || local.rfind("/media/", 0) == 0)
        {
            local.erase(0, local.find_first_not_of('/'));
            local = local.substr(0, local.find_first_of("?#"));
        }
        if (local.empty() || local.rfind("data:", 0) == 0 || local.rfind("blob:", 0) == 0)
            return {};
        return (directory / std::filesystem::u8path(local)).lexically_normal();
    }
}

bool shadertoy_project::import_json(const path &file, path &manifest, std::string &error)
try
{
    const auto input = std::filesystem::absolute(file).lexically_normal();
    auto text = read(input, 32 * 1024 * 1024);
    if (text.compare(0, 3, "\xef\xbb\xbf") == 0)
        text.erase(0, 3);
    // cJSON exposes zero-terminated strings; disallow escaped NUL rather than
    // silently truncating source, paths, or IDs. A literal \\u0000 remains valid.
    for (size_t i = 0; i < text.size(); ++i)
        if (text[i] == '\\')
        {
            require(text.compare(i + 1, 5, "u0000") != 0, "JSON strings cannot contain escaped NUL characters.");
            ++i;
        }
    const char *end = nullptr;
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> document(cJSON_ParseWithLengthOpts(text.c_str(), text.size() + 1, &end, true), cJSON_Delete);
    require(document != nullptr, "Invalid Shadertoy JSON near byte " + std::to_string(end ? end - text.c_str() : 0) + '.');
    validate_json(document.get());
    json shader = document.get();
    for (int nesting = 0; nesting < 4; ++nesting)
    {
        if (cJSON_IsArray(shader))
        {
            require(cJSON_GetArraySize(shader) == 1, "JSON must contain exactly one shader; export one shader from the collection.");
            shader = cJSON_GetArrayItem(shader, 0);
        }
        else if (field(shader, "Shader"))
            shader = field(shader, "Shader");
        else if (field(shader, "shaders"))
            shader = field(shader, "shaders");
        else
            break;
    }
    require(cJSON_IsObject(shader), "Expected a Shadertoy shader object.");
    if (auto api_error = field(shader, "Error"))
        throw std::runtime_error("Shadertoy API error: " + string(api_error, "Error"));
    auto renderpasses = field(shader, "renderpass");
    require(cJSON_IsArray(renderpasses) && cJSON_GetArraySize(renderpasses) >= 1 && cJSON_GetArraySize(renderpasses) <= 8,
            "Shadertoy JSON needs 1 to 8 renderpass entries.");
    shadertoy_project next;
    if (auto info = field(shader, "info"))
    {
        require(cJSON_IsObject(info), "info must be an object.");
        next.title = optional_string(info, "name");
        auto author = optional_string(info, "username"), id = optional_string(info, "id");
        require(next.title.size() <= 4096 && author.size() <= 4096 && id.size() <= 256, "Shadertoy metadata is too large.");
        if (!author.empty())
            next.import_notes.push_back("Original author: " + author);
        if (!id.empty())
            next.import_notes.push_back("Shadertoy ID: " + id);
    }
    if (next.title.empty())
        next.title = input.stem().u8string();
    next.import_notes.push_back("Imported from " + input.filename().u8string() + ". The original JSON is unchanged.");
    std::vector<imported_pass> parsed;
    std::array<bool, 8> occupied{};
    for (json p = renderpasses->child; p; p = p->next)
    {
        require(cJSON_IsObject(p), "Each renderpass must be an object.");
        auto type = string(field(p, "type"), "Pass type");
        int slot = type == "image" ? 5 : type == "sound" ? 6
                                     : type == "common"  ? 7
                                     : type == "cubemap" ? 4
                                     : type == "buffer"  ? buffer_slot(optional_string(p, "name"))
                                                         : -2;
        require(slot != -2, "Unsupported Shadertoy pass type: " + type);
        if (slot >= 0)
        {
            require(!occupied[slot], "Duplicate Shadertoy pass: " + type);
            occupied[slot] = true;
        }
        std::string code;
        if (auto inline_code = field(p, "code"))
            code = string(inline_code, "Pass code");
        else
        {
            auto code_file = string(field(p, "codeFile"), "Pass code or codeFile");
            require(!code_file.empty() && code_file.find("://") == code_file.npos, "codeFile must reference a local GLSL file.");
            code = read(input.parent_path() / std::filesystem::u8path(code_file), 4 * 1024 * 1024);
        }
        require(code.size() <= 4 * 1024 * 1024, "Pass source exceeds 4 MiB.");
        std::string output;
        if (auto outputs = field(p, "outputs"))
        {
            require(cJSON_IsArray(outputs) && cJSON_GetArraySize(outputs) <= 1, "A pass supports at most one output.");
            if (auto out = outputs->child)
            {
                require(cJSON_IsObject(out), "Pass output must be an object.");
                if (auto channel = field(out, "channel"))
                    integer(channel, 0, 0, "Output channel");
                output = identifier(field(out, "id"));
            }
        }
        parsed.push_back({p, slot, std::move(code), output});
    }
    std::map<std::string, int> outputs;
    for (auto &p : parsed)
    {
        if (p.slot == -1)
        {
            auto free = std::find(occupied.begin(), occupied.begin() + 4, false);
            require(free != occupied.begin() + 4, "Shadertoy supports at most four buffers.");
            p.slot = static_cast<int>(free - occupied.begin());
            occupied[p.slot] = true;
        }
        if (!p.output.empty())
            require(outputs.emplace(p.output, p.slot).second, "Duplicate output ID: " + p.output);
        // Temporary canonical filenames allow structural validation before writing anything.
        if (p.slot == 7)
            next.common = "Common.glsl";
        else
            next.passes[p.slot].source = std::string(toy_pass_name(static_cast<toy_pass_kind>(p.slot))) + ".glsl";
    }
    for (const auto &p : parsed)
    {
        auto inputs = field(p.object, "inputs");
        if (!inputs)
            continue;
        require(cJSON_IsArray(inputs) && cJSON_GetArraySize(inputs) <= 4, "A pass supports at most four inputs.");
        require(p.slot != 7 || !inputs->child, "Common cannot have input channels.");
        std::array<bool, 4> assigned{};
        for (json in = inputs->child; in; in = in->next)
        {
            require(cJSON_IsObject(in), "Channel input must be an object.");
            int index = integer(field(in, "channel"), 0, 3, "Input channel");
            require(!assigned[index], "Duplicate input channel " + std::to_string(index));
            assigned[index] = true;
            auto &channel = next.passes[p.slot].channels[index];
            auto type = optional_string(in, "ctype", optional_string(in, "type"));
            auto id = identifier(field(in, "id"));
            auto output = outputs.find(id);
            if (type == "buffer" || (type == "cubemap" && output != outputs.end()))
            {
                require(output != outputs.end() && output->second <= 4, "Unresolved buffer/cubemap input ID: " + id);
                require((type == "cubemap") == (output->second == 4), "Input sampler type does not match output ID: " + id);
                channel.kind = static_cast<toy_input_kind>(output->second + 1);
            }
            else if (type == "texture")
                channel.kind = toy_input_kind::texture;
            else if (type == "cubemap")
                channel.kind = toy_input_kind::cube_texture;
            else if (type == "volume")
                channel.kind = toy_input_kind::volume;
            else if (type == "keyboard")
            {
                channel.kind = toy_input_kind::keyboard;
                channel.filter = 0;
            }
            else if (type == "music" || type == "musicstream" || type == "audio")
                channel.kind = toy_input_kind::audio;
            else if (type == "mic" || type == "microphone")
                channel.kind = toy_input_kind::microphone;
            else if (type == "video")
                channel.kind = toy_input_kind::video;
            else if (type == "webcam" || type == "camera")
                channel.kind = toy_input_kind::camera;
            else
                throw std::runtime_error("Unsupported Shadertoy input type: " + type);
            channel.vflip = false;
            if (auto sampler = field(in, "sampler"))
            {
                require(cJSON_IsObject(sampler), "Channel sampler must be an object.");
                auto filter = optional_string(sampler, "filter", channel.filter == 0 ? "nearest" : "linear");
                require(filter == "nearest" || filter == "linear" || filter == "mipmap", "Unknown sampler filter: " + filter);
                channel.filter = filter == "nearest" ? 0 : filter == "linear" ? 1
                                                                              : 2;
                auto wrap = optional_string(sampler, "wrap", "clamp");
                require(wrap == "clamp" || wrap == "repeat", "Unknown sampler wrap: " + wrap);
                channel.wrap = wrap == "repeat" ? 1 : 0;
                channel.vflip = flag(sampler, "vflip", false);
                channel.srgb = flag(sampler, "srgb", false);
            }
            channel.origin = optional_string(in, "src", optional_string(in, "filepath"));
            require(channel.origin.size() <= 8192, "Channel source reference exceeds 8192 bytes.");
            bool asset = channel.kind == toy_input_kind::texture || channel.kind == toy_input_kind::cube_texture ||
                         channel.kind == toy_input_kind::volume || channel.kind == toy_input_kind::audio || channel.kind == toy_input_kind::video;
            if (asset)
            {
                channel.file = asset_path(channel.origin, input.parent_path());
                auto label = std::string(toy_pass_name(static_cast<toy_pass_kind>(p.slot))) + " / iChannel" + std::to_string(index);
                if (channel.file.empty() || !std::filesystem::exists(channel.file))
                    next.import_notes.push_back(label + ": choose a local asset for " + (channel.origin.empty() ? "the missing source reference" : channel.origin) + ".");
                if (channel.kind == toy_input_kind::cube_texture)
                    next.import_notes.push_back(label + ": Studio requires a horizontal six-face cubemap strip.");
                if (channel.kind == toy_input_kind::volume)
                    next.import_notes.push_back(label + ": use a horizontal volume atlas and set its slice count; Shadertoy .bin volumes need conversion.");
                if (channel.kind == toy_input_kind::audio)
                    next.import_notes.push_back(label + ": audio must be a local WAV; online streams and other audio formats need conversion.");
            }
        }
    }
    if (!next.validate(error))
        return false;
    // Reserve a fresh sibling directory. JSON names and IDs never become filenames.
    path directory;
    for (int n = 0; n < 10000; ++n)
    {
        auto candidate = input.parent_path() / (input.stem().u8string() + "-import" + (n ? "-" + std::to_string(n) : ""));
        std::error_code ec;
        if (std::filesystem::create_directory(candidate, ec))
        {
            directory = candidate;
            break;
        }
        if (ec && ec != std::errc::file_exists)
            throw std::filesystem::filesystem_error("Cannot create JSON import directory", candidate, ec);
    }
    require(!directory.empty(), "Cannot find an unused JSON import directory.");
    std::vector<path> written;
    try
    {
        for (const auto &p : parsed)
        {
            auto &source = p.slot == 7 ? next.common : next.passes[p.slot].source;
            source = directory / source;
            written.push_back(source);
            require(shader_save_source(source, p.code, error), error);
        }
        auto preset = directory / "project.stoy";
        written.push_back(preset);
        require(next.save(preset, error), error);
        *this = std::move(next);
        manifest = preset;
        error.clear();
        return true;
    }
    catch (...)
    {
        // Remove only files created by this import, without recursive deletion.
        std::error_code ignored;
        for (const auto &created : written)
        {
            std::filesystem::remove(created, ignored);
            auto temporary = created;
            temporary += ".tmp";
            std::filesystem::remove(temporary, ignored);
        }
        std::filesystem::remove(directory, ignored);
        throw;
    }
}
catch (const std::exception &e)
{
    error = e.what();
    return false;
}
