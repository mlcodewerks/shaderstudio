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
  main.cpp preview.cpp preview.h project.cpp project.h project_smoke.cpp
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
set(STUDIO_LOCAL_FILES ${STUDIO_LOCAL_SOURCES}
  CMakeLists.txt source_files.cmake package_source.cmake package_source_smoke.cmake
  static_curl.cmake verify_static_curl.cmake
  README.md THIRD_PARTY_NOTICES.md CURL-LICENSE.txt
  IMGUI-LICENSE.txt GLAD-LICENSE.txt DIRENT-LICENSE.txt)

list(TRANSFORM STUDIO_MODEL_SHARED_SOURCES PREPEND "${STUDIO_SOURCE_ROOT}/")
list(TRANSFORM STUDIO_APP_SHARED_SOURCES PREPEND "${STUDIO_SOURCE_ROOT}/")
