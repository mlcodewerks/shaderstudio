#include <SDL3/SDL.h>
#include <features/features_cpu.h>
#include <file/file_path.h>
#include <cstring>

// Supply the decoder subset's platform hooks through Studio's existing SDL
// dependency, without pulling in libretro-common's filesystem/frontend layer.
extern "C" uint64_t cpu_features_get(void)
{
    uint64_t flags = 0;
    if (SDL_HasSSE2()) flags |= RETRO_SIMD_SSE2;
    if (SDL_HasNEON()) flags |= RETRO_SIMD_NEON;
    return flags;
}
extern "C" const char *path_get_extension(const char *path)
{
    if (!path) return "";
    const char *extension = "";
    for (const char *p = path; *p; ++p)
    {
        if (*p == '/' || *p == '\\') extension = "";
        else if (*p == '.') extension = p + 1;
    }
    return extension;
}
