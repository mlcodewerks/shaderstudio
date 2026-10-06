// Keep the shared backend unchanged; only the libretro host needs a lost-context path.
#define IMGUI_IMPL_OPENGL_LOADER_CUSTOM
#include "imgui_impl_opengl3.cpp"

void studio_imgui_abandon_context()
{
    auto *data = ImGui_ImplOpenGL3_GetBackendData();
    if (!data) return;
    data->ShaderHandle = data->VboHandle = data->ElementsHandle = 0;
#ifdef IMGUI_IMPL_OPENGL_MAY_HAVE_BIND_SAMPLER
    data->TexSamplers[0] = data->TexSamplers[1] = 0;
#endif
    for (auto *texture : ImGui::GetPlatformIO().Textures)
    {
        texture->SetTexID(ImTextureID_Invalid);
        texture->SetStatus(ImTextureStatus_Destroyed);
    }
    // No GL calls are valid after a context loss (or from retro_deinit).
    auto &io = ImGui::GetIO();
    io.BackendRendererName = nullptr;
    io.BackendRendererUserData = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
    ImGui::GetPlatformIO().ClearRendererHandlers();
    IM_DELETE(data);
}
