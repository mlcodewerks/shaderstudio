#pragma once
#include <filesystem>
#include <vector>

// Decode and resample into the sampler's interleaved stereo float format.
// Throws on unsupported input, decode errors, or the decoded-size limit.
std::vector<float> studio_decode_audio(const std::filesystem::path &, unsigned output_rate = 44100);
