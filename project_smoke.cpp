#include "project.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <fstream>
#include <stdexcept>

static void require(bool ok, const std::string &error)
{
    if (!ok)
        throw std::runtime_error(error);
}
static std::string read_text(const std::filesystem::path &file)
{
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
static void json_smoke(const std::filesystem::path &root)
{
    std::string error;
    const std::string shader = R"({"ver":"0.1","info":{"name":"JSON \u2603 project",
    "username":"Fixture author","id":"Test01"},"renderpass":[
      {"type":"image","code":"void mainImage(out vec4 c,in vec2 p) {\n c=vec4(1);\n}\n","inputs":[
        {"channel":3,"ctype":"buffer","id":"12","sampler":{"filter":"mipmap","wrap":"repeat","vflip":"true","srgb":false}},
        {"channel":1,"ctype":"cubemap","id":"cube"},
        {"channel":0,"ctype":"texture","src":"/media/test.png","sampler":{"vflip":true,"srgb":"true"}},
        {"channel":2,"ctype":"keyboard"}]},
      {"type":"buffer","name":"Buffer B","outputs":[{"id":12,"channel":0}],"code":"// Buffer B\n",
      "inputs":[{"channel":0,"ctype":"buffer","id":"A"}]},
      {"type":"common","code":"// Common: \u03bb\n"},
      {"type":"cubemap","outputs":[{"id":"cube","channel":0}],"code":"// Cubemap\n",
      "inputs":[{"channel":0,"type":"cubemap","filepath":"https://www.shadertoy.com/media/cube.png"}]},
      {"type":"buffer","name":"Buffer A","outputs":[{"id":"A"}],"code":"// Buffer A\n","inputs":[{"channel":0,"ctype":"buffer","id":"A"}]},
      {"type":"sound","code":"vec2 mainSound(int s,float t) { return vec2(0); }","inputs":[
        {"channel":0,"ctype":"music","src":"track.wav"},{"channel":1,"ctype":"mic"},
        {"channel":2,"ctype":"webcam"},{"channel":3,"ctype":"video","src":"clip.mp4"}]}]})";
    std::filesystem::create_directories(root / "media");
    require(shader_save_source(root / "media/test.png", "fixture", error), error);
    auto file = root / "fixture.json";
    require(shader_save_source(file, "{\"Shader\":" + shader + "}", error), error);
    auto original = read_text(file);
    // Import must avoid existing files as well as existing directories.
    require(shader_save_source(root / "fixture-import", "keep", error), error);
    studio_project loaded;
    require(loaded.load(file, error), error);
    require(loaded.is_shadertoy && loaded.preset.filename() == "project.stoy" && !loaded.dirty, "JSON did not create an editable project");
    require(loaded.chain.name == u8"JSON ☃ project", "JSON Unicode title was not decoded");
    require(read_text(loaded.toy.common) == u8"// Common: λ\n", "Common source was not decoded exactly");
    require(read_text(loaded.toy.passes[5].source) == "void mainImage(out vec4 c,in vec2 p) {\n c=vec4(1);\n}\n", "JSON GLSL was rewritten");
    require(loaded.toy.passes[5].channels[3].kind == toy_input_kind::buffer_b &&
                loaded.toy.passes[1].channels[0].kind == toy_input_kind::buffer_a &&
                loaded.toy.passes[0].channels[0].kind == toy_input_kind::buffer_a,
            "JSON output IDs did not resolve shuffled passes and feedback");
    const auto &channels = loaded.toy.passes[5].channels;
    require(channels[1].kind == toy_input_kind::cubemap && channels[2].kind == toy_input_kind::keyboard && channels[2].filter == 0, 
        "Channel slots or sampler types were lost");
    require(channels[3].filter == 2 && channels[3].wrap == 1 && channels[3].vflip && !channels[3].srgb && channels[0].srgb,
         "Sampler options were lost");
    require(channels[0].file == root / "media/test.png" && channels[0].origin == "/media/test.png",
         "Website asset was not resolved beside JSON");
    require(loaded.toy.passes[4].channels[0].kind == toy_input_kind::cube_texture &&
                loaded.toy.passes[4].channels[0].file == root / "media/cube.png",
            "External cubemap was confused with a render pass");
    const auto &sound = loaded.toy.passes[6].channels;
    require(sound[0].kind == toy_input_kind::audio && sound[1].kind == toy_input_kind::microphone &&
                sound[2].kind == toy_input_kind::camera && sound[3].kind == toy_input_kind::video,
            "Media channel types were lost");
    require(!loaded.toy.import_notes.empty(), "Missing remote assets have no import notes");
    studio_project restored;
    require(restored.load(loaded.preset, error), error);
    require(restored.chain.name == loaded.chain.name && restored.toy.import_notes == loaded.toy.import_notes &&
                restored.toy.passes[5].channels[0].origin == channels[0].origin,
            "Imported metadata was lost on reopen");
    require(read_text(file) == original && read_text(root / "fixture-import") == "keep", "JSON import overwrote existing data");
    auto first = loaded.preset;
    require(loaded.load(file, error) && loaded.preset != first, "Repeated JSON import overwrote the first import");
    for (const auto &body : {shader, "[" + shader + "]", "{\"shaders\":[" + shader + "]}"})
    {
        require(shader_save_source(root / "variant.JSON", body, error), error);
        require(loaded.load(root / "variant.JSON", error), error);
    }
    require(shader_save_source(root / "external.glsl", "// external GLSL\n", error), error);
    require(shader_save_source(root / "external.json", R"({"renderpass":[{"type":"image","codeFile":"external.glsl"}]})", error), error);
    require(loaded.load(root / "external.json", error), error);
    require(read_text(loaded.toy.passes[5].source) == "// external GLSL\n", "codeFile source was not read");
    const std::string unnamed = R"({"renderpass":[{"type":"buffer","outputs":[{"id":100}],"code":"// unnamed"},
        {"type":"buffer","name":"Buffer A","outputs":[{"id":200}],"code":"// A"},
        {"type":"image","code":"// Image","inputs":[{"channel":0,"ctype":"buffer","id":100}]}]})";
    require(shader_save_source(root / "unnamed.json", unnamed, error), error);
    require(loaded.load(root / "unnamed.json", error), error);
    require(loaded.toy.passes[5].channels[0].kind == toy_input_kind::buffer_b, "Unnamed buffer stole a named buffer's slot");
    auto active = loaded.preset;
    const std::vector<std::string> invalid = {
        "{", shader + " trailing", "[]", "[" + shader + ',' + shader + ']',
        R"({"Error":"Shader not found"})",
        R"({"renderpass":[{"type":"image","code":4}]})",
        R"({"renderpass":[{"type":"image","code":"\u0000truncated"}]})",
        R"({"renderpass":[{"type":"image","code":"first","code":"second"}]})",
        R"({"renderpass":[{"type":"common","code":"// no image"}]})",
        R"({"renderpass":[{"type":"image","code":""},{"type":"image","code":""}]})",
        R"({"renderpass":[{"type":"image","code":"","inputs":[{"channel":4,"ctype":"keyboard"}]}]})",
        R"({"renderpass":[{"type":"image","code":"","inputs":[{"channel":0.5,"ctype":"keyboard"}]}]})",
        R"({"renderpass":[{"type":"image","code":"","inputs":[{"channel":0,"ctype":"buffer","id":999}]}]})",
        R"({"renderpass":[{"type":"image","code":"","inputs":[{"channel":0,"ctype":"keyboard"},{"channel":0,"ctype":"keyboard"}]}]})",
        R"({"renderpass":[{"type":"buffer","code":"","outputs":[{"id":1}]},{"type":"image","code":"","outputs":[{"id":1}]}]})",
        R"({"renderpass":[{"type":"image","code":"","inputs":[{"channel":0,"ctype":"unknown"}]}]})",
        R"({"renderpass":[{"type":"image","code":"","inputs":[{"channel":0,"ctype":"keyboard","sampler":{"vflip":"invalid"}}]}]})",
        R"({"renderpass":[{"type":"image","codeFile":"missing.glsl"}]})"};
    for (const auto &body : invalid)
    {
        require(shader_save_source(root / "invalid.json", body, error), error);
        require(!loaded.load(root / "invalid.json", error) && !error.empty() && loaded.preset == active, "Invalid JSON replaced the active project");
        require(!std::filesystem::exists(root / "invalid-import"), "Invalid JSON left an import directory behind");
    }
    std::puts("PASS: Shadertoy JSON wrappers, Unicode/source extraction, output-ID graph, samplers/media, metadata round trip, non-overwrite and invalid-input retention");
}
int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    try
    {
        auto root = std::filesystem::absolute(std::filesystem::u8path(argv[1])) / std::to_string(SDL_GetPerformanceCounter());
        studio_project project;
        std::string error;
        require(project.create(root / "new project", shader_template::image, error), error);
        auto source = project.chain.passes[0].source;
        require(std::filesystem::exists(project.preset) && std::filesystem::exists(source), "New project files were not created");
        require(!project.create(root / "new project", shader_template::image, error), "Existing project was overwritten");
        require(project.chain.passes[0].source == source, "Failed project creation replaced the active project");
        require(project.add_pass(shader_template::procedural, error), error);
        require(project.chain.passes.size() == 2 && project.chain.passes[1].source != source, "Adding a pass overwrote an existing source");
        project.chain.values["GAIN"] = 0.75f;
        project.chain.passes[1].options["scale"] = "2.0";
        require(project.save(project.preset, error), error);
        studio_project restored;
        require(restored.load(project.preset, error), error);
        require(restored.chain.passes.size() == 2 && restored.chain.values["GAIN"] == 0.75f &&
                    restored.chain.passes[1].options["scale"] == "2.0",
                "Preset round trip lost pass options or parameters");
        project.chain.passes[0].enabled = false;
        require(!project.save(project.preset, error), "Saving silently dropped a bypassed pass");
        require(!restored.load(root / "missing.slangp", error) && restored.chain.passes.size() == 2, "Failed open lost the active project");
        require(restored.load(source, error) && restored.preset.empty() && restored.dirty, "Opening a raw shader would overwrite it with preset data");
        require(!restored.save(source, error), "Preset save accepted a shader source filename");
        require(restored.save(root / "from-source.slangp", error), error);
        auto parameters = shader_pass_parameters(source);
        require(parameters.size() == 1 && parameters[0] == "GAIN", "Starter parameter declaration is missing");
        studio_project toy;
        require(toy.create(root / "toy project", shader_template::shadertoy, error), error);
        require(toy.is_shadertoy && toy.preset.extension() == ".stoy" && toy.chain.passes.size() == 2, "Shadertoy starter is incomplete");
        require(toy.toy.add(toy.preset.parent_path(), toy_pass_kind::buffer_a, error), error);
        auto &channel = toy.toy.passes[5].channels[2];
        channel.kind = toy_input_kind::buffer_a;
        channel.filter = 2;
        channel.wrap = 1;
        channel.vflip = false;
        toy.toy.vr = true;
        require(toy.save(toy.preset, error), error);
        studio_project loaded;
        require(loaded.load(toy.preset, error), error);
        require(loaded.is_shadertoy && loaded.toy.vr && loaded.toy.passes[5].channels[2].filter == 2 &&
                    loaded.toy.passes[5].channels[2].kind == toy_input_kind::buffer_a,
                "Shadertoy round trip lost options");
        auto moved = root / "moved toy";
        std::filesystem::copy(toy.preset.parent_path(), moved, std::filesystem::copy_options::recursive);
        require(loaded.load(moved / "project.stoy", error), error);
        require(loaded.toy.passes[5].source.parent_path() == moved, "Shadertoy project paths are not portable");
        require(!loaded.save(moved / "Image.glsl", error), "Shadertoy save overwrote GLSL source");
        auto malformed = root / "bad.stoy";
        require(shader_save_source(malformed, "SHADERTOY_STUDIO 1\nchannel 100 0 0 \"\" 1 0 1 0 1 30\n", error), error);
        require(!loaded.load(malformed, error) && loaded.toy.passes[5].source.parent_path() == moved, "Malformed project replaced active project");
        require(shader_save_source(malformed, "SHADERTOY_STUDIO 1\nvr", error), error);
        require(!loaded.load(malformed, error), "Truncated project accepted");
        loaded.toy.passes[5].channels[0].kind = toy_input_kind::buffer_d;
        require(!loaded.save(loaded.preset, error), "Missing buffer reference accepted");
        require(loaded.load(toy.toy.passes[5].source, error) && loaded.preset.empty() && loaded.dirty && loaded.is_shadertoy, "Raw GLSL import failed");
        require(loaded.save(root / "raw.stoy", error), error);
        json_smoke(root);
        std::puts("PASS: scratch project/templates, non-overwrite, pass creation, preset round trip, failed-load retention and raw shader save safety");
        return 0;
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
