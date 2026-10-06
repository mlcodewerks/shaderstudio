#include "shadertoy_download.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace
{
    void require(bool ok, const std::string &message)
    {
        if (!ok)
            throw std::runtime_error(message);
    }
    std::string read(const std::filesystem::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }
}
int main(int argc, char **argv)
try
{
    if (argc == 2 && std::string(argv[1]) == "--probe-live")
    {
        std::atomic_bool cancel{false};
        studio_http_response response;
        std::string error;
        if (!studio_curl_fetch({"https://www.shadertoy.com/shadertoy", "s=%7B%22shaders%22%3A%5B%22MdX3zr%22%5D%7D&nt=1&nl=1&np=1"}, response, cancel, error))
        {
            std::fprintf(stderr, "Live Shadertoy request: %s\n", error.c_str());
            return 1;
        }
        require(response.content_type.find("json") != response.content_type.npos, "Live endpoint did not return JSON.");
        std::puts("PASS: static libcurl performed a verified HTTPS Shadertoy request");
        return 0;
    }
    if (argc != 2)
        return 2;
    auto root = std::filesystem::absolute(std::filesystem::u8path(argv[1])) / std::to_string(SDL_GetPerformanceCounter());
    std::filesystem::create_directories(root);
    std::string id, error;
    for (const std::string url : {"Ab12Cd", "https://www.shadertoy.com/view/Ab12Cd", "http://shadertoy.com/embed/Ab12Cd?gui=true",
                                  "shadertoy.com/view/Ab12Cd/", "https://www.shadertoy.com/api/v1/shaders/Ab12Cd",
                                   " https://www.shadertoy.com/view/Ab12Cd#test "})
        require(studio_shadertoy_id(url, id, error) && id == "Ab12Cd", "Valid Shadertoy URL rejected");
    for (const std::string url : {"", "abcdefg", "https://evil.test/view/Ab12Cd", "https://shadertoy.com.evil.test/view/Ab12Cd",
                                  "https://user@shadertoy.com/view/Ab12Cd", "file:///view/Ab12Cd",
                                   "https://shadertoy.com/view/Ab12Cd/more"})
        require(!studio_shadertoy_id(url, id, error), "Invalid Shadertoy URL accepted");
    const std::string shader = R"({"Shader":{"info":{"name":"Download fixture"},"renderpass":[{"type":"image",
        "code":"void mainImage(out vec4 c,in vec2 p) { c=vec4(1); }","inputs":[
        {"channel":0,"ctype":"texture","src":"/media/fixture.png"},
        {"channel":1,"ctype":"texture","src":"/media/fixture.png"},
        {"channel":2,"ctype":"texture","src":"https://external.test/private.png"},
        {"channel":3,"ctype":"texture","src":"C:/private/local.png"}]}]}})";
    std::atomic_bool cancel{false};
    studio_project project;
    int requests = 0;
    auto fetch = [&](const studio_http_request &request, studio_http_response &response, const std::atomic_bool &, std::string &)
    {
        ++requests;
        require(request.url.rfind("https://www.shadertoy.com/", 0) == 0, "Unexpected download host");
        if (request.url.find("/media/") != request.url.npos)
            response = {"image fixture", "image/png"};
        else
        {
            require(request.url == "https://www.shadertoy.com/api/v1/shaders/Ab12Cd?key=fixture" && request.post.empty(), "Incorrect API request");
            response = {shader, "application/json"};
        }
        return true;
    };
    require(!studio_download_shadertoy("Ab12Cd", "", root / "no-key", true, project, cancel, error, fetch) 
    && error == "Shadertoy requires an API key." && requests == 0 && !std::filesystem::exists(root / "no-key"),
            "Missing API key was not rejected before downloading");
    require(studio_download_shadertoy("https://www.shadertoy.com/view/Ab12Cd", "fixture", root / "download",
         true, project, cancel, error, fetch), error);
    require(requests == 2, "Duplicate assets were downloaded more than once or external assets were fetched");
    auto &channels = project.toy.passes[5].channels;
    require(channels[0].file == channels[1].file && read(channels[0].file) == "image fixture",
     "Downloaded channel assets were not connected");
    require(channels[2].file.empty() && channels[3].file.empty(), "Remote shader can reference local or unsupported external files");
    studio_project reopened;
    require(reopened.load(project.preset, error), error);
    require(reopened.toy.passes[5].channels[0].file == channels[0].file, "Downloaded asset path was lost on save");
    auto active = project.preset;
    require(!studio_download_shadertoy("Ab12Cd", "fixture", root / "download", true, project, cancel, error, fetch) && project.preset == active && requests == 2,
            "Download overwrote existing project or issued a needless request");
    auto api = [&](const studio_http_request &request, studio_http_response &response, const std::atomic_bool &, std::string &)
    {
        require(request.url == "https://www.shadertoy.com/api/v1/shaders/Ab12Cd?key=secret%26%3F%23" && request.post.empty(),
         "API key was not escaped or wrong endpoint used");
        response = {shader, "application/json"};
        return true;
    };
    require(studio_download_shadertoy("Ab12Cd", "secret&?#", root / "api", false, project, cancel, error, api), error);
    require(read(project.preset).find("secret") == std::string::npos, "API key leaked into the project");
    active = project.preset;
    auto fail = [](const studio_http_request &, studio_http_response &, const std::atomic_bool &, std::string &error)
    { error="HTTP 403 fixture"; return false; };
    require(!studio_download_shadertoy("Ab12Cd", "fixture", root / "failed", true, project, cancel, error, fail) && 
    project.preset == active && !std::filesystem::exists(root / "failed"), "HTTP failure lost project or left a directory");
    for (const auto &body : {std::string("<html>Access denied</html>"), 
        std::string(R"({"renderpass":[{"type":"image","codeFile":"C:/private/source.glsl"}]})"), std::string(R"({"Error":"Shader not found"})")})
    {
        auto invalid = [&](const studio_http_request &, studio_http_response &response, const std::atomic_bool &, std::string &)
        { response={body,"application/json"}; return true; };
        require(!studio_download_shadertoy("Ab12Cd", "fixture", root / "invalid", true, project, cancel, error, invalid) && 
        project.preset == active && !std::filesystem::exists(root / "invalid"), "Invalid server response changed the project");
    }
    auto cancel_asset = [&](const studio_http_request &request, studio_http_response &response, const std::atomic_bool &, std::string &error)
    {
        if (request.url.find("/media/") != request.url.npos)
        {
            cancel = true;
            error = "cancelled";
            return false;
        }
        response = {shader, "application/json"};
        return true;
    };
    require(!studio_download_shadertoy("Ab12Cd", "fixture", root / "cancel", true, project, cancel, error, cancel_asset) && 
    project.preset == active && !std::filesystem::exists(root / "cancel"), "Cancellation left a partial project");
    studio_http_response response;
    require(!studio_curl_fetch({"https://www.shadertoy.com/", {}}, response, cancel, error) && error == "Download cancelled.", 
    "Real libcurl transport ignored cancellation");
    cancel = false;
    require(!studio_curl_fetch({"http://www.shadertoy.com/", {}}, response, cancel, error), "Real transport accepted insecure HTTP");
    std::puts("PASS: URL parsing, requests/API escaping, downloaded assets/deduplication, local-path isolation, "
              "save/reopen, failure retention and cancellation");
    return 0;
}
catch (const std::exception &e)
{
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
}
