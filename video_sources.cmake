set(STUDIO_VIDEO_SOURCES
  "${SRC}/deps/libretro-common/formats/audio_transfer.c"
  "${SRC}/deps/libretro-common/formats/wav/rwav.c"
  "${SRC}/deps/libretro-common/formats/flac/rflac.c"
  "${SRC}/deps/libretro-common/formats/vorbis/rvorbis.c"
  "${SRC}/deps/libretro-common/formats/mp3/rmp3.c"
  "${SRC}/deps/libretro-common/formats/opus/ropus.c"
  "${SRC}/deps/libretro-common/formats/aac/raac.c"
  "${SRC}/deps/libretro-common/formats/mod/rmodtracker.c"
  "${SRC}/deps/libretro-common/formats/ac3/rac3_decode.c"
  "${SRC}/deps/libretro-common/formats/ac3/rac3_frame.c"
  "${SRC}/deps/libretro-common/encodings/encoding_crc32.c"
  "${SRC}/deps/libretro-common/formats/mp4/rmp4.c"
  "${SRC}/deps/libretro-common/formats/mp4/rmp4_video.c"
  "${SRC}/deps/libretro-common/formats/webm/rwebm.c"
  "${SRC}/deps/libretro-common/formats/webm/rwebm_video.c"
  "${SRC}/deps/libretro-common/formats/vp8/rvp8.c"
  "${SRC}/deps/libretro-common/formats/vp9/rvp9.c"
  "${SRC}/deps/libretro-common/formats/h264/rh264.c"
  "${SRC}/deps/libretro-common/formats/h265/rh265.c"
  "${SRC}/deps/libretro-common/formats/image/image_hdr_blit.c"
  "${SRC}/deps/libretro-common/formats/image/image_blit_bands.c"
)
add_library(studio-video STATIC video_decoder.cpp audio_decoder.cpp media_support.cpp ${STUDIO_VIDEO_SOURCES})
target_include_directories(studio-video PRIVATE "${SRC}/deps/libretro-common/include")
target_compile_definitions(studio-video PRIVATE HAVE_RVP9 HAVE_RWAV HAVE_RFLAC HAVE_RVORBIS HAVE_RMP3 HAVE_ROPUS HAVE_RAAC HAVE_RMODTRACKER HAVE_RAC3 HAVE_RMP4 HAVE_RWEBM)
set_target_properties(studio-video PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(studio-video PUBLIC SDL3::SDL3)

add_executable(shader-studio-media-smoke media_smoke.cpp)
target_link_libraries(shader-studio-media-smoke PRIVATE studio-video)
add_test(NAME shader-studio-media-smoke COMMAND shader-studio-media-smoke "${CMAKE_CURRENT_SOURCE_DIR}/test-media")

if(UNIX)
  target_link_libraries(studio-video PUBLIC m)
endif()
