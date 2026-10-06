# Shared by the build and source exporter. Paths describe the exported tree.
# Keep this explicit: never distribute build output, user projects, or unrelated
# dependencies just because they happen to be present in the working checkout.
if(IS_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/src")
  set(STUDIO_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}")
else()
  get_filename_component(STUDIO_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
endif()
set(SRC "${STUDIO_SOURCE_ROOT}/src")

set(STUDIO_MODEL_SHARED_SOURCES
  src/deps/misc/cJSON.c
  src/main/shader_source_io.cpp
  src/main/shader_chain.cpp
  src/main/shader_library.cpp
  src/main/shader_diagnostics.cpp)
set(STUDIO_APP_SHARED_SOURCES
  src/main/shader_runtime.cpp
  src/main/shader_gl.cpp
  src/main/shader_source_editor.cpp
  src/deps/misc/glad.cpp
  src/deps/misc/ImGuiFileDialog.cpp
  src/deps/ImGuiColorTextEdit/TextEditor.cpp
  src/deps/imgui/imgui.cpp
  src/deps/imgui/imgui_draw.cpp
  src/deps/imgui/imgui_tables.cpp
  src/deps/imgui/imgui_widgets.cpp
  src/deps/imgui/imgui_impl_sdl3.cpp
  src/deps/imgui/imgui_impl_opengl3.cpp)
set(STUDIO_LOCAL_SOURCES
  libretro_core.cpp libretro_imgui.cpp libretro_smoke.cpp
  media_smoke.cpp video_decoder.cpp video_decoder.h audio_decoder.cpp audio_decoder.h media_support.cpp
  main.cpp workspace.h workspace_smoke.cpp preview.cpp preview.h project.cpp project.h project_smoke.cpp
  shadertoy_project.cpp shadertoy_project.h shadertoy_json.cpp
  shadertoy_runtime.cpp shadertoy_runtime.h shadertoy_smoke.cpp
  shadertoy_download.cpp shadertoy_download.h download_smoke.cpp)
set(STUDIO_SHARED_FILES
  ${STUDIO_MODEL_SHARED_SOURCES}
  ${STUDIO_APP_SHARED_SOURCES}
  LICENSE
  src/main/shader_chain.h
  src/main/shader_library.h
  src/main/shader_diagnostics.h
  src/main/shader_runtime.h
  src/main/shader_gl.h
  src/main/shader_source_editor.h
  src/deps/misc/cJSON.h
  src/deps/misc/glad.h
  src/deps/misc/libretro.h
  src/deps/misc/khrplatform.h
  src/deps/misc/ImGuiFileDialog.h
  src/deps/misc/ImGuiFileDialogConfig.h
  src/deps/misc/dirent/dirent.h
  src/deps/ImGuiColorTextEdit/TextEditor.h
  src/deps/ImGuiColorTextEdit/LICENSE
  src/deps/librashader/librashader.h
  src/deps/stb/stb_image.h
  src/deps/imgui/imgui.h
  src/deps/imgui/imconfig.h
  src/deps/imgui/imgui_internal.h
  src/deps/imgui/imstb_rectpack.h
  src/deps/imgui/imstb_textedit.h
  src/deps/imgui/imstb_truetype.h
  src/deps/imgui/imgui_impl_sdl3.h
  src/deps/imgui/imgui_impl_opengl3.h)
list(APPEND STUDIO_SHARED_FILES
  src/deps/libretro-common/LICENSES.txt
  src/deps/libretro-common/encodings/encoding_crc32.c
  src/deps/libretro-common/encodings/encoding_crc32_tables.h
  src/deps/libretro-common/formats/aac/raac.c
  src/deps/libretro-common/formats/ac3/rac3_decode.c
  src/deps/libretro-common/formats/ac3/rac3_frame.c
  src/deps/libretro-common/formats/ac3/rac3_tables.h
  src/deps/libretro-common/formats/audio_transfer.c
  src/deps/libretro-common/formats/flac/rflac.c
  src/deps/libretro-common/formats/flac/rflac_internal.h
  src/deps/libretro-common/formats/h264/rh264.c
  src/deps/libretro-common/formats/h264/rh264_bd.inc
  src/deps/libretro-common/formats/h265/rh265.c
  src/deps/libretro-common/formats/h265/rh265_bd.inc
  src/deps/libretro-common/formats/image/image_blit_bands.c
  src/deps/libretro-common/formats/image/image_hdr_blit.c
  src/deps/libretro-common/formats/mod/rmodtracker.c
  src/deps/libretro-common/formats/mp3/rmp3.c
  src/deps/libretro-common/formats/mp4/rmp4.c
  src/deps/libretro-common/formats/mp4/rmp4_video.c
  src/deps/libretro-common/formats/opus/ropus.c
  src/deps/libretro-common/formats/vorbis/rvorbis.c
  src/deps/libretro-common/formats/vp8/rvp8.c
  src/deps/libretro-common/formats/vp9/rvp9.c
  src/deps/libretro-common/formats/vp9/rvp9_recon.inc
  src/deps/libretro-common/formats/wav/rwav.c
  src/deps/libretro-common/formats/webm/rwebm.c
  src/deps/libretro-common/formats/webm/rwebm_video.c
  src/deps/libretro-common/include/boolean.h
  src/deps/libretro-common/include/compat/intrinsics.h
  src/deps/libretro-common/include/compat/msvc.h
  src/deps/libretro-common/include/compat/posix_string.h
  src/deps/libretro-common/include/compat/strcasestr.h
  src/deps/libretro-common/include/compat/strl.h
  src/deps/libretro-common/include/encodings/crc32.h
  src/deps/libretro-common/include/features/features_cpu.h
  src/deps/libretro-common/include/file/file_path.h
  src/deps/libretro-common/include/formats/audio.h
  src/deps/libretro-common/include/formats/image.h
  src/deps/libretro-common/include/formats/image_blit_bands.h
  src/deps/libretro-common/include/formats/raac.h
  src/deps/libretro-common/include/formats/rac3.h
  src/deps/libretro-common/include/formats/rflac.h
  src/deps/libretro-common/include/formats/rh264.h
  src/deps/libretro-common/include/formats/rh265.h
  src/deps/libretro-common/include/formats/rlpcm.h
  src/deps/libretro-common/include/formats/rmodtracker.h
  src/deps/libretro-common/include/formats/rmp3.h
  src/deps/libretro-common/include/formats/rmp4.h
  src/deps/libretro-common/include/formats/rmp4_video.h
  src/deps/libretro-common/include/formats/ropus.h
  src/deps/libretro-common/include/formats/rvorbis.h
  src/deps/libretro-common/include/formats/rvp8.h
  src/deps/libretro-common/include/formats/rvp9.h
  src/deps/libretro-common/include/formats/rwav.h
  src/deps/libretro-common/include/formats/rwebm.h
  src/deps/libretro-common/include/formats/rwebm_video.h
  src/deps/libretro-common/include/libretro.h
  src/deps/libretro-common/include/retro_atomic.h
  src/deps/libretro-common/include/retro_common_api.h
  src/deps/libretro-common/include/retro_endianness.h
  src/deps/libretro-common/include/retro_inline.h
  src/deps/libretro-common/include/retro_miscellaneous.h
  src/deps/libretro-common/include/rthreads/retro_eventcount.h
  src/deps/libretro-common/include/rthreads/rthreads.h
  src/deps/libretro-common/include/rthreads/tpool.h
  src/deps/libretro-common/include/string/stdstring.h
  src/deps/libretro-common/UPSTREAM.txt
)
set(STUDIO_LOCAL_FILES ${STUDIO_LOCAL_SOURCES}
  test-media/README.md
  test-media/colors.mp4
  test-media/colors.webm
  test-media/tone.aac
  test-media/tone.ac3
  test-media/tone.flac
  test-media/tone.m4a
  test-media/tone.mp3
  test-media/tone.ogg
  test-media/tone.opus
  test-media/tone.wav

  Makefile Makefile.shaderstudio Makefile.shadertoy_libretro
  video_sources.cmake CMakeLists.txt source_files.cmake package_source.cmake package_source_smoke.cmake
  libretro.cmake shadertoy_libretro.info
  static_curl.cmake verify_static_curl.cmake
  README.md THIRD_PARTY_NOTICES.md CURL-LICENSE.txt
  IMGUI-LICENSE.txt GLAD-LICENSE.txt DIRENT-LICENSE.txt)

list(TRANSFORM STUDIO_MODEL_SHARED_SOURCES PREPEND "${STUDIO_SOURCE_ROOT}/")
list(TRANSFORM STUDIO_APP_SHARED_SOURCES PREPEND "${STUDIO_SOURCE_ROOT}/")
