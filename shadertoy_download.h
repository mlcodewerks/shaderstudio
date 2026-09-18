#pragma once
#include "project.h"
#include <atomic>
#include <functional>

struct studio_http_request
{
    std::string url, post;
    size_t limit = 32 * 1024 * 1024;
};
struct studio_http_response
{
    std::string body, content_type;
};
using studio_http_fetch = std::function<bool(const studio_http_request &, studio_http_response &,
                                             const std::atomic_bool &, std::string &)>;

bool studio_shadertoy_id(const std::string &url, std::string &id, std::string &error);
bool studio_curl_fetch(const studio_http_request &, studio_http_response &, const std::atomic_bool &, std::string &);
// Destination must be a new folder. Only a completed download replaces result.
bool studio_download_shadertoy(const std::string &url, const std::string &api_key,
                               const std::filesystem::path &destination, bool assets, studio_project &result,
                               const std::atomic_bool &cancel, std::string &error, const studio_http_fetch &fetch = studio_curl_fetch);
