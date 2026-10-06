#include "audio_decoder.h"
#include "video_decoder.h"
#include <cmath>
#include <cstdio>
#include <stdexcept>

static void require(bool ok, const char *message)
{
    if (!ok) throw std::runtime_error(message);
}
int main(int argc, char **argv)
try
{
    require(argc == 2, "Pass the test-media directory");
    const std::filesystem::path directory = std::filesystem::u8path(argv[1]);
    for (const char *name : {"colors.mp4", "colors.webm"})
    {
        studio_video_decoder decoder;
        studio_video_frame frame;
        decoder.open(directory / name);
        require(decoder.update(0, frame), "No first video frame");
        require(frame.w == 32 && frame.h == 32 && frame.pixels[0] > 200 && frame.pixels[1] < 30, "First frame must be red");
        require(!decoder.update(0, frame), "Paused video advanced");
        decoder.update(0.6, frame);
        require(frame.pixels[1] > 200 && frame.pixels[0] < 30, "Video did not advance to green");
        decoder.update(1.1, frame);
        require(frame.pixels[0] > 200 && frame.pixels[1] < 30, "Video did not loop to red");
        decoder.update(0.6, frame);
        require(frame.pixels[1] > 200, "Backward seek failed");
        decoder.restart();
        decoder.update(0, frame);
        require(frame.pixels[0] > 200, "Video did not restart");
        std::printf("PASS: %s timing, pause, loop, seek and restart\n", name);
    }
    for (const char *name : {"tone.wav", "tone.flac", "tone.mp3", "tone.ogg", "tone.opus", "tone.m4a", "tone.aac", "tone.ac3"})
    {
        auto audio = studio_decode_audio(directory / name);
        require(audio.size() >= 15000 && audio.size() <= 24000 && audio.size() % 2 == 0, "Incorrect resampled audio length");
        double energy = 0;
        for (auto sample : audio)
        {
            require(std::isfinite(sample), "Non-finite audio sample");
            energy += sample * sample;
        }
        require(energy / audio.size() > 0.0001, "Decoded audio is silent");
        std::printf("PASS: %s decoding and 48 kHz -> 44.1 kHz stereo conversion\n", name);
    }
    bool rejected = false;
    try { studio_decode_audio(directory / "colors.mp4"); } catch (...) { rejected = true; }
    require(rejected, "Video without audio was not rejected");
    rejected = false;
    try { studio_video_decoder decoder; decoder.open(directory / "tone.wav"); } catch (...) { rejected = true; }
    require(rejected, "Audio file was accepted as video");
    return 0;
}
catch (const std::exception &e)
{
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
