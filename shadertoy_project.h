#pragma once
#include "shader_chain.h"
#include <array>

// Slot order is the execution order; a reference to a later slot reads last frame.
enum class toy_pass_kind
{
    buffer_a,
    buffer_b,
    buffer_c,
    buffer_d,
    cubemap,
    image,
    sound
};
enum class toy_input_kind
{
    none,
    buffer_a,
    buffer_b,
    buffer_c,
    buffer_d,
    cubemap,
    texture,
    cube_texture,
    volume,
    keyboard,
    audio,
    microphone,
    video,
    camera
};
struct toy_channel
{
    toy_input_kind kind = toy_input_kind::none;
    std::filesystem::path file;
    int filter = 1; // nearest, linear, mipmap
    int wrap = 0;   // clamp, repeat
    bool vflip = true, srgb = false;
    int depth = 1;      // number of slices in a horizontal volume atlas
    float fps = 30;     // numbered image sequence
    std::string origin; // Original JSON asset reference, retained when remapping locally.
};
struct toy_pass
{
    std::filesystem::path source;
    std::array<toy_channel, 4> channels;
};
struct shadertoy_project
{
    std::string title;
    std::vector<std::string> import_notes;
    std::filesystem::path common;
    std::array<toy_pass, 7> passes;
    bool vr = false;
    bool load(const std::filesystem::path &, std::string &);
    bool import_json(const std::filesystem::path &, std::filesystem::path &manifest, std::string &error);
    bool save(const std::filesystem::path &, std::string &) const;
    bool add(const std::filesystem::path &directory, toy_pass_kind, std::string &);
    shader_chain_config editor_chain(const std::string &name) const;
    bool validate(std::string &) const;
};
const char *toy_pass_name(toy_pass_kind);
const char *toy_input_name(toy_input_kind);
std::string toy_template(toy_pass_kind);
