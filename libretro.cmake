set(STUDIO_LIBRETRO_SOURCES
  libretro_core.cpp libretro_imgui.cpp project.cpp preview.cpp
  shadertoy_project.cpp shadertoy_runtime.cpp shadertoy_json.cpp shadertoy_download.cpp
  "${SRC}/deps/misc/cJSON.c"
  "${SRC}/main/shader_chain.cpp" "${SRC}/main/shader_library.cpp"
  "${SRC}/main/shader_source_io.cpp" "${SRC}/main/shader_diagnostics.cpp"
  "${SRC}/main/shader_source_editor.cpp"
  "${SRC}/deps/misc/ImGuiFileDialog.cpp"
  "${SRC}/deps/misc/glad.cpp" "${SRC}/deps/ImGuiColorTextEdit/TextEditor.cpp"
  "${SRC}/deps/imgui/imgui.cpp" "${SRC}/deps/imgui/imgui_draw.cpp"
  "${SRC}/deps/imgui/imgui_tables.cpp" "${SRC}/deps/imgui/imgui_widgets.cpp")
add_library(shadertoy_libretro SHARED ${STUDIO_LIBRETRO_SOURCES})
target_include_directories(shadertoy_libretro PRIVATE
  "${SRC}/main" "${SRC}/deps/misc" "${SRC}/deps/stb"
  "${SRC}/deps/imgui" "${SRC}/deps/ImGuiColorTextEdit")
target_link_libraries(shadertoy_libretro PRIVATE studio-video SDL3::SDL3 ${CMAKE_DL_LIBS} studio-curl Threads::Threads)
set_target_properties(shadertoy_libretro PROPERTIES PREFIX ""
  CXX_VISIBILITY_PRESET hidden C_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
install(TARGETS shadertoy_libretro LIBRARY DESTINATION lib/libretro RUNTIME DESTINATION lib/libretro)
install(FILES shadertoy_libretro.info DESTINATION share/libretro/info)
add_executable(shader-studio-libretro-smoke libretro_smoke.cpp "${SRC}/deps/misc/glad.cpp")
target_include_directories(shader-studio-libretro-smoke PRIVATE "${SRC}/deps/misc")
target_link_libraries(shader-studio-libretro-smoke PRIVATE SDL3::SDL3 ${CMAKE_DL_LIBS})
add_test(NAME shader-studio-libretro-api COMMAND shader-studio-libretro-smoke
  "$<TARGET_FILE:shadertoy_libretro>" "${CMAKE_CURRENT_BINARY_DIR}/libretro-api-output" api)
if(STUDIO_GPU_TESTS)
  foreach(api IN ITEMS gl gles)
    add_test(NAME shader-studio-libretro-${api} COMMAND shader-studio-libretro-smoke
      "$<TARGET_FILE:shadertoy_libretro>" "${CMAKE_CURRENT_BINARY_DIR}/libretro-${api}-output" ${api})
    set_tests_properties(shader-studio-libretro-${api} PROPERTIES TIMEOUT 60)
  endforeach()
endif()
