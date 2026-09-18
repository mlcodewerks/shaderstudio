#pragma once
#include "shader_chain.h"
#include "shadertoy_project.h"

enum class shader_template
{
    image,
    procedural,
    shadertoy
};
std::string studio_shader_template(shader_template kind);

struct studio_project
{
    std::filesystem::path preset;
    shader_chain_config chain;
    bool dirty = false;
    bool is_shadertoy = false;
    shadertoy_project toy;
    void sync_toy() { chain = toy.editor_chain(chain.name); }
    bool create(const std::filesystem::path &new_directory, shader_template, std::string &error);
    bool load(const std::filesystem::path &, std::string &error);
    bool add_pass(shader_template, std::string &error);
    bool save(const std::filesystem::path &, std::string &error);
};
