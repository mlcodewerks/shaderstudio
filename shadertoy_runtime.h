#pragma once
#include "shadertoy_project.h"
#include "shader_diagnostics.h"
#include <memory>
#include <functional>

class shadertoy_runtime
{
public:
    shadertoy_runtime();
    ~shadertoy_runtime(); // GL context must still be current.
    bool compile(const shadertoy_project &);
    unsigned render(unsigned width, unsigned height, float delta);
    void reset();
    void clear();
    void abandon_context(); // Release CPU state after a lost GL context; never delete old GL names.
    void key(unsigned code, bool down, bool repeat = false);
    void release_keys();
    void mouse(float x, float y, bool down, bool clicked);
    void set_playing(bool);
    bool sound_enabled = false, capture_enabled = false;
    // When supplied, send interleaved stereo at 44100 Hz to the host instead of SDL.
    std::function<void(const float *, size_t)> audio_output;
    unsigned texture = 0;
    std::string error, media_status;
    shader_diagnostic_report diagnostics;
    float time() const;
    int frame() const;

private:
    struct state;
    std::unique_ptr<state> current;
    std::array<unsigned char, 256 * 3> keys{};
    std::array<float, 4> pointer{};
    bool playing = true;
};
