#include "shadertoy_download.h"
#include "cJSON.h"
#include <curl/curl.h>
#include <algorithm>
#include <fstream>
#include <memory>
#include <regex>
#include <set>
#include <stdexcept>

namespace
{
    void require(bool ok, const std::string &message)
    {
        if (!ok)
            throw std::runtime_error(message);
    }
    bool cancelled(const std::atomic_bool &cancel) { return cancel.load(std::memory_order_relaxed); }
    void check_cancel(const std::atomic_bool &cancel) { require(!cancelled(cancel), "Download cancelled."); }
    bool hosted(const std::string &url)
    {
        static const std::regex pattern(R"(^https://(www\.)?shadertoy\.com/[^\s\\]*$)", std::regex::icase);
        return std::regex_match(url, pattern);
    }
    std::string escaped(const std::string &value)
    {
        static const char hex[] = "0123456789ABCDEF";
        std::string result;
        for (unsigned char c : value)
        {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
                result += c;
            else
            {
                result += '%';
                result += hex[c >> 4];
                result += hex[c & 15];
            }
        }
        return result;
    }
    struct curl_global
    {
        CURLcode status = curl_global_init(CURL_GLOBAL_DEFAULT);
        ~curl_global()
        {
            if (status == CURLE_OK)
                curl_global_cleanup();
        }
    };
    struct transfer
    {
        std::string body;
        size_t limit;
        const std::atomic_bool &cancel;
        bool oversized = false, failed = false;
    };
    size_t write_body(char *data, size_t size, size_t count, void *userdata) noexcept
    {
        auto &state = *static_cast<transfer *>(userdata);
        if (cancelled(state.cancel))
            return 0;
        if (size && count > (state.limit - state.body.size()) / size)
        {
            state.oversized = true;
            return 0;
        }
        size_t bytes = size * count;
        try
        {
            state.body.append(data, bytes);
            return bytes;
        }
        catch (...)
        {
            state.failed = true;
            return 0;
        }
    }
    int progress(void *userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept
    {
        return cancelled(*static_cast<const std::atomic_bool *>(userdata)) ? 1 : 0;
    }
    void inline_sources(const cJSON *node, int depth = 0)
    {
        require(depth <= 64, "Downloaded JSON is nested too deeply.");
        for (auto child = node->child; child; child = child->next)
        {
            require(!child->string || std::string(child->string) != "codeFile", "Downloaded shaders must contain inline code, not local codeFile references.");
            inline_sources(child, depth + 1);
        }
    }
    std::string asset_url(const std::string &origin)
    {
        auto url = origin.rfind("/media/", 0) == 0 ? "https://www.shadertoy.com" + origin : origin;
        if (!hosted(url))
            return {};
        auto begin = url.find('/', url.find("://") + 3);
        if (url.compare(begin, 7, "/media/") != 0)
            return {};
        return url;
    }
    bool supported_asset(toy_input_kind type, const std::string &extension)
    {
        if (type == toy_input_kind::texture)
            return extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".bmp" || extension == ".tga";
        if (type == toy_input_kind::audio)
            return extension == ".wav";
        if (type == toy_input_kind::video)
            return extension == ".mp4" || extension == ".webm" || extension == ".mkv" || extension == ".mov" || extension == ".avi" || extension == ".gif";
        return false; // Website cubemaps/volumes require conversion to Studio's atlases.
    }
}

bool studio_shadertoy_id(const std::string &value, std::string &id, std::string &error)
{
    static const std::regex raw(R"(^[A-Za-z0-9]{6}$)");
    static const std::regex url(R"(^(?:https?://)?(?:www\.)?shadertoy\.com/(?:view|embed|api/v1/shaders)/([A-Za-z0-9]{6})/?(?:[?#][^\s]*)?$)", std::regex::icase);
    auto start = value.find_first_not_of(" \t\r\n"), end = value.find_last_not_of(" \t\r\n");
    auto text = start == value.npos ? std::string{} : value.substr(start, end - start + 1);
    std::smatch match;
    if (std::regex_match(text, raw))
        id = text;
    else if (std::regex_match(text, match, url))
        id = match[1];
    else
    {
        error = "Enter a Shadertoy view/embed URL or a six-character shader ID.";
        return false;
    }
    error.clear();
    return true;
}

bool studio_curl_fetch(const studio_http_request &request, studio_http_response &response,
                       const std::atomic_bool &cancel, std::string &error)
try
{
    static curl_global global;
    require(global.status == CURLE_OK, "Cannot initialize libcurl.");
    require(hosted(request.url), "Downloads must use HTTPS on shadertoy.com.");
    require(request.limit > 0 && request.limit <= 64 * 1024 * 1024, "Invalid download size limit.");
    std::string url = request.url, post = request.post;
    for (int redirects = 0; redirects <= 3; ++redirects)
    {
        check_cancel(cancel);
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
        require(curl != nullptr, "Cannot create libcurl request.");
        transfer state{{}, request.limit, cancel};
        auto *handle = curl.get();
        curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
        curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "https");
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(handle, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 128L);
        curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, 15L);
        curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(handle, CURLOPT_USERAGENT, "WTFweg-Shader-Studio/1.0");
        curl_easy_setopt(handle, CURLOPT_REFERER, "https://www.shadertoy.com/");
        curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
#ifdef _WIN32
        curl_easy_setopt(handle, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
        curl_easy_setopt(handle, CURLOPT_CAINFO, nullptr);
#endif
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_body);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &state);
        curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, progress);
        curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &cancel);
        curl_easy_setopt(handle, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(request.limit));
        if (!post.empty())
        {
            curl_easy_setopt(handle, CURLOPT_POSTFIELDS, post.c_str());
            curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(post.size()));
        }
        auto code = curl_easy_perform(handle);
        check_cancel(cancel);
        require(!state.oversized && code != CURLE_FILESIZE_EXCEEDED, "Download exceeds the size limit.");
        require(!state.failed, "Cannot allocate download memory.");
        require(code == CURLE_OK, std::string("Download failed: ") + curl_easy_strerror(code));
        long status = 0;
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
        if (status >= 300 && status < 400)
        {
            char *next = nullptr;
            curl_easy_getinfo(handle, CURLINFO_REDIRECT_URL, &next);
            require(next && hosted(next) && redirects < 3, "Shadertoy returned an unsupported redirect.");
            if (status == 301 || status == 302 || status == 303)
                post.clear();
            url = next;
            continue;
        }
        require(status == 200, "Shadertoy returned HTTP " + std::to_string(status) +
                                   ". Check shader visibility or try an API key / JSON export.");
        char *type = nullptr;
        curl_easy_getinfo(handle, CURLINFO_CONTENT_TYPE, &type);
        studio_http_response next{std::move(state.body), type ? type : ""};
        response = std::move(next);
        error.clear();
        return true;
    }
    throw std::runtime_error("Too many redirects.");
}
catch (const std::exception &e)
{
    error = e.what();
    return false;
}

bool studio_download_shadertoy(const std::string &url, const std::string &api_key,
                               const std::filesystem::path &destination, bool assets, studio_project &result,
                               const std::atomic_bool &cancel, std::string &error, const studio_http_fetch &fetch)
try
{
    std::string id;
    if (!studio_shadertoy_id(url, id, error))
        return false;
    require(api_key.size() <= 512, "API key is too long.");
    check_cancel(cancel);
    auto directory = std::filesystem::absolute(destination).lexically_normal();
    require(!std::filesystem::exists(directory), "Choose a new download folder; that path already exists.");
    studio_http_request request;
    require(!api_key.empty(), "Shadertoy requires an API key.");
    request.url = "https://www.shadertoy.com/api/v1/shaders/" + id + "?key=" + escaped(api_key);
    studio_http_response response;
    require(fetch(request, response, cancel, error), error);
    check_cancel(cancel);
    require(response.body.size() <= request.limit, "Shader JSON exceeds 32 MiB.");
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(cJSON_ParseWithLengthOpts(response.body.c_str(), response.body.size() + 1, nullptr, true), cJSON_Delete);
    require(json != nullptr && response.content_type.find("text/html") == std::string::npos,
            "Shadertoy did not return shader JSON. It requires a API key; use a JSON export if unavailable.");
    inline_sources(json.get());
    require(std::filesystem::create_directory(directory), "Cannot create the download folder.");
    std::vector<std::filesystem::path> files, directories{directory};
    try
    {
        auto source = directory / (id + ".json");
        files.push_back(source);
        // JSON can exceed the GLSL writer's 4 MiB limit.
        std::ofstream out(source, std::ios::binary);
        out.write(response.body.data(), response.body.size());
        out.close();
        require(static_cast<bool>(out), "Cannot save downloaded shader JSON.");
        studio_project next;
        require(next.load(source, error), error);
        directories.push_back(next.preset.parent_path());
        files.push_back(next.preset);
        for (const auto &pass : next.chain.passes)
            files.push_back(pass.source);
        next.toy.import_notes.push_back("Downloaded from https://www.shadertoy.com/view/" + id);
        // Import notes about missing assets are regenerated after attempting downloads.
        auto &notes = next.toy.import_notes;
        notes.erase(std::remove_if(notes.begin(), notes.end(), [](const std::string &n)
                                   { return n.find(": choose a local asset for ") != n.npos; }),
                    notes.end());
        std::map<std::string, std::filesystem::path> downloaded;
        size_t total = 0;
        for (int i = 0; i < 7; ++i)
            for (int j = 0; j < 4; ++j)
            {
                check_cancel(cancel);
                auto &channel = next.toy.passes[i].channels[j];
                bool media = channel.kind == toy_input_kind::texture || channel.kind == toy_input_kind::cube_texture ||
                             channel.kind == toy_input_kind::volume || channel.kind == toy_input_kind::audio || channel.kind == toy_input_kind::video;
                if (!media)
                    continue;
                channel.file.clear(); // Never read local paths named by remote JSON.
                auto location = asset_url(channel.origin);
                auto extension = std::filesystem::u8path(location.substr(0, location.find_first_of("?#"))).extension().u8string();
                std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c)
                               { return std::tolower(c); });
                std::string failure = "choose a local asset (external, disabled, or conversion required).";
                if (assets && !location.empty() && supported_asset(channel.kind, extension))
                {
                    auto found = downloaded.find(location);
                    if (found != downloaded.end())
                        channel.file = found->second;
                    else
                    {
                        studio_http_response asset;
                        if (total < 256 * 1024 * 1024 && fetch({location, {}, std::min<size_t>(64 * 1024 * 1024, 256 * 1024 * 1024 - total)}, asset, cancel, failure))
                        {
                            require(asset.body.size() <= 64 * 1024 * 1024 && total + asset.body.size() <= 256 * 1024 * 1024, "Asset downloads exceed the size limit.");
                            if (asset.content_type.find("text/html") == asset.content_type.npos && !asset.body.empty())
                            {
                                auto target = next.preset.parent_path() / ("asset-" + std::to_string(downloaded.size()) + extension);
                                files.push_back(target);
                                std::ofstream binary(target, std::ios::binary);
                                binary.write(asset.body.data(), asset.body.size());
                                binary.close();
                                require(static_cast<bool>(binary), "Cannot save downloaded asset.");
                                downloaded[location] = channel.file = target;
                                total += asset.body.size();
                            }
                            else
                                failure = "server returned no media; choose a local asset.";
                        }
                    }
                }
                if (channel.file.empty())
                    notes.push_back(std::string(toy_pass_name(static_cast<toy_pass_kind>(i))) + " / iChannel" + std::to_string(j) + ": " + failure);
            }
        check_cancel(cancel);
        require(next.save(next.preset, error), error);
        result = std::move(next);
        error.clear();
        return true;
    }
    catch (...)
    {
        std::error_code ignored;
        for (auto at = files.rbegin(); at != files.rend(); ++at)
        {
            std::filesystem::remove(*at, ignored);
            auto temp = *at;
            temp += ".tmp";
            std::filesystem::remove(temp, ignored);
        }
        for (auto at = directories.rbegin(); at != directories.rend(); ++at)
            std::filesystem::remove(*at, ignored);
        throw;
    }
}
catch (const std::exception &e)
{
    error = e.what();
    return false;
}
