cmake_minimum_required(VERSION 3.24)

set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "")

# Off Apple the dependencies in vcpkg.json come from vcpkg (ADR 0016); the macOS build does not use it.
if(CMAKE_HOST_WIN32 AND DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

project(nocturne VERSION 0.1.1 LANGUAGES CXX)
if(APPLE)
  enable_language(OBJCXX)
endif()

# ccache, when installed, so a branch switch or a fresh build folder reuses what was compiled before.
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  foreach(lang C CXX OBJC OBJCXX)
    if(NOT CMAKE_${lang}_COMPILER_LAUNCHER)
      set(CMAKE_${lang}_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    endif()
  endforeach()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(MSVC)
  # clang-cl: /W4 is clang's -Wall -Wextra (its /Wall would be -Weverything).
  add_compile_options(/W4 -Wno-unused-parameter -Wno-missing-designated-field-initializers /utf-8)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
endif()

option(NOCTURNE_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(NOCTURNE_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize=vptr -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined -fno-sanitize=vptr)
endif()

execute_process(COMMAND ${CMAKE_COMMAND} -E env TZ=UTC git log -1 --format=%cd-%h --date=format-local:%Y%m%d%H%M%S
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} OUTPUT_VARIABLE NOCTURNE_BUILD_ID OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND git rev-parse --absolute-git-dir
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} OUTPUT_VARIABLE git_dir OUTPUT_STRIP_TRAILING_WHITESPACE)
if(EXISTS "${git_dir}/logs/HEAD")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${git_dir}/logs/HEAD")
endif()
configure_file(src/protocol/build_info.cpp.in build_info.cpp @ONLY)

option(NOCTURNE_TRACY "Instrument the engine for the Tracy profiler" OFF)
add_library(nocturne_profile INTERFACE)
if(NOCTURNE_TRACY)
  include(FetchContent)
  set(TRACY_ENABLE ON)
  set(TRACY_ON_DEMAND ON)
  FetchContent_Declare(tracy
    GIT_REPOSITORY https://github.com/wolfpld/tracy.git
    GIT_TAG v0.14.1
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(tracy)
  target_link_libraries(nocturne_profile INTERFACE Tracy::TracyClient)
  target_compile_definitions(nocturne_profile INTERFACE NOCTURNE_TRACY)
endif()

include(FetchContent)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
set(ENABLE_PCH OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(glslang
  URL https://github.com/KhronosGroup/glslang/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz
  URL_HASH SHA256=cda24ec765e6d2845d106b7caa593f34127195a88524cf87821ff0088fb6edec
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(spirv_cross
  URL https://github.com/KhronosGroup/SPIRV-Cross/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz
  URL_HASH SHA256=f708982e88b763ef5b0394ed468fc2c3628f68cdab89f2e7a946cd628c04e721
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(glslang spirv_cross)
foreach(dep glslang glslang-default-resource-limits spirv-cross-core spirv-cross-glsl spirv-cross-msl)
  target_compile_options(${dep} PRIVATE -w)
endforeach()

add_library(nocturne_shader_cross STATIC src/engine/shader_cross.cpp src/engine/isf_compiler.cpp)
target_include_directories(nocturne_shader_cross PUBLIC src)
target_link_libraries(nocturne_shader_cross PUBLIC nocturne_protocol PRIVATE glslang::glslang
  glslang::glslang-default-resource-limits spirv-cross-msl)

file(GLOB isf_files CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/isf/*/*.fs)
list(SORT isf_files)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${isf_files})
set(isf_entries "")
foreach(file ${isf_files})
  get_filename_component(name ${file} NAME_WLE)
  file(READ ${file} text)
  string(APPEND isf_entries "        {\"${name}\", R\"isf(${text})isf\"},\n")
endforeach()
configure_file(src/effects/isf_sources.cpp.in isf_sources.cpp @ONLY)

add_subdirectory(src/platform)

add_library(nocturne_protocol STATIC
  ${CMAKE_CURRENT_BINARY_DIR}/build_info.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/isf_sources.cpp
  src/document/composition.cpp
  src/document/clip_edits.cpp
  src/document/composition_edits.cpp
  src/document/edit_support.cpp
  src/document/effect_edits.cpp
  src/document/grid_edits.cpp
  src/document/layer_edits.cpp
  src/document/screen_edits.cpp
  src/document/screen_roles.cpp
  src/document/composition_json.cpp
  src/document/edit_json.cpp
  src/document/mapping.cpp
  src/document/cues.cpp
  src/document/lineup.cpp
  src/net/lan.cpp
  src/net/pairing.cpp
  src/net/socket_compat.cpp
  src/document/wall_clock.cpp
  src/document/modulation.cpp
  src/document/composition_file.cpp
  src/document/text.cpp
  src/document/palette.cpp
  src/effects/chain_planner.cpp
  src/effects/classic_presets.cpp
  src/effects/colour.cpp
  src/effects/effect_registry.cpp
  src/effects/isf.cpp
  src/effects/preset_service.cpp
  src/import/xml.cpp
  src/protocol/bundle_push.cpp
  src/protocol/document_mirror.cpp
  src/protocol/document_server.cpp
  src/protocol/messages.cpp
  src/protocol/prepare.cpp
  src/protocol/remote_files.cpp
  src/protocol/return_stream.cpp
  src/supervisor/crash_guard.cpp
  src/supervisor/rotating_log.cpp
  src/supervisor/state_saver.cpp
)
target_include_directories(nocturne_protocol PUBLIC src)
target_link_libraries(nocturne_protocol PUBLIC nocturne_platform)
target_include_directories(nocturne_protocol SYSTEM PUBLIC third_party/nlohmann)
target_sources(nocturne_protocol PRIVATE src/protocol/bundle_push.cpp)
if(WIN32)
  target_sources(nocturne_protocol PRIVATE src/document/composition_file_win32.cpp)
  target_link_libraries(nocturne_protocol PUBLIC ws2_32 iphlpapi advapi32)
else()
  target_sources(nocturne_protocol PRIVATE src/document/composition_file_posix.cpp)
endif()

# Engine logic with no GPU, media or OS dependency: it builds on every platform.
add_library(nocturne_engine_logic STATIC
  src/engine/autopilot_stepper.cpp
  src/engine/bundle_update.cpp
  src/engine/machine_actions.cpp
  src/engine/transport.cpp
)
target_link_libraries(nocturne_engine_logic PUBLIC nocturne_protocol nocturne_profile)
if(WIN32)
  target_sources(nocturne_engine_logic PRIVATE src/engine/bundle_update_win32.cpp)
endif()

# pocketfft is the spectrum's transform off Apple; on Apple the tests compare it with vDSP. Same commit as vcpkg's port.
if(APPLE)
  FetchContent_Declare(pocketfft
    URL https://github.com/mreineck/pocketfft/archive/0fa0ef591e38c2758e3184c6c23e497b9f732ffa.tar.gz
    URL_HASH SHA256=1cfd0494ac6fe88e034579cb655b0ef9731b39a0432558e50fe41c4abd6d8684
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(pocketfft)
  set(POCKETFFT_INCLUDE_DIR "${pocketfft_SOURCE_DIR}")
else()
  find_path(POCKETFFT_INCLUDE_DIR pocketfft_hdronly.h REQUIRED)
endif()

add_library(nocturne_audio STATIC
  src/audio/audio_analysis.cpp
  src/audio/audio_analyzer.cpp
  src/audio/audio_device_settings.cpp
  src/audio/audio_input.cpp
  src/audio/audio_service.cpp
  src/audio/return_audio.cpp
)
target_include_directories(nocturne_audio PUBLIC src)
target_link_libraries(nocturne_audio PUBLIC nocturne_protocol)
if(APPLE)
  target_sources(nocturne_audio PRIVATE src/audio/audio_input.mm src/audio/opus_codec.cpp src/audio/power_spectrum_vdsp.cpp)
  target_link_libraries(nocturne_audio PUBLIC
    "-framework Accelerate"
    "-framework AudioToolbox"
    "-framework AVFoundation"
    "-framework CoreAudio"
    "-framework Foundation"
  )
else()
  find_package(Opus CONFIG REQUIRED)
  target_sources(nocturne_audio PRIVATE src/audio/opus_codec_libopus.cpp src/audio/power_spectrum_pocketfft.cpp)
  target_include_directories(nocturne_audio PRIVATE "${POCKETFFT_INCLUDE_DIR}")
  target_link_libraries(nocturne_audio PRIVATE Opus::opus)
endif()
if(WIN32)
  target_sources(nocturne_audio PRIVATE src/audio/audio_input_win32.cpp)
  target_link_libraries(nocturne_audio PRIVATE ole32 avrt mmdevapi)
endif()

# Media: HAP, the decoders' frame caches, the Media Library and text. Reading and writing movies, stills and text
# rasterising use AVFoundation and CoreText on Apple, FFmpeg and DirectWrite elsewhere (ADR 0016).
add_library(nocturne_media STATIC
  src/media/hap.cpp
  src/media/hap_encoder.cpp
  src/media/keep_local.cpp
  src/media/library_service.cpp
  src/media/media_library.cpp
  src/media/snappy.cpp
  src/media/text_renderer.cpp
  src/media/video_decoder.cpp
)
target_include_directories(nocturne_media PUBLIC src)
target_link_libraries(nocturne_media PUBLIC nocturne_protocol nocturne_profile)
if(APPLE)
  target_sources(nocturne_media PRIVATE
    src/media/clip_decoders.cpp
    src/media/composition_media.cpp
    src/media/composition_service.cpp
    src/media/hap_movie.mm
    src/media/hap_texture.mm
    src/media/stills.mm
    src/media/text_renderer_coretext.cpp
    src/media/video_source_avf.mm
  )
  target_include_directories(nocturne_media PUBLIC third_party/metal-cpp)
  target_link_libraries(nocturne_media PUBLIC
    "-framework AVFoundation"
    "-framework CoreMedia"
    "-framework CoreVideo"
    "-framework CoreText"
    "-framework CoreGraphics"
    "-framework ImageIO"
    "-framework Metal"
    "-framework Foundation"
  )
else()
  find_package(FFMPEG REQUIRED)
  target_sources(nocturne_media PRIVATE
    src/media/hap_movie_ffmpeg.cpp
    src/media/stills_ffmpeg.cpp
    src/media/video_source_ffmpeg.cpp
  )
  target_include_directories(nocturne_media PRIVATE ${FFMPEG_INCLUDE_DIRS})
  target_link_directories(nocturne_media PUBLIC ${FFMPEG_LIBRARY_DIRS})
  target_link_libraries(nocturne_media PUBLIC ${FFMPEG_LIBRARIES})
endif()
if(WIN32)
  target_sources(nocturne_media PRIVATE src/media/text_renderer_dwrite.cpp)
  target_link_libraries(nocturne_media PUBLIC d2d1 dwrite ole32 windowscodecs)
endif()

# Return video's packet rules; its H.264 encoder and decoder are added with recording's encoder below.
add_library(nocturne_return_video STATIC src/output/return_video.cpp)
target_link_libraries(nocturne_return_video PUBLIC nocturne_protocol)

# The protocol's WebSocket: Network.framework on Apple, plain sockets elsewhere.
add_library(nocturne_net STATIC src/net/websocket_frames.cpp src/net/websocket_rules.cpp)
target_link_libraries(nocturne_net PUBLIC nocturne_protocol nocturne_return_video)
if(APPLE)
  target_sources(nocturne_net PRIVATE src/net/websocket.mm)
  target_link_libraries(nocturne_net PUBLIC "-framework Network" objc)
else()
  target_sources(nocturne_net PRIVATE src/net/websocket_sockets.cpp)
endif()

# The UDP clock (ADR 0005): Network.framework on Apple, plain sockets elsewhere.
add_library(nocturne_udp_clock STATIC src/sync/clock_estimator.cpp)
target_include_directories(nocturne_udp_clock PUBLIC src)
target_link_libraries(nocturne_udp_clock PUBLIC nocturne_protocol)
if(APPLE)
  target_sources(nocturne_udp_clock PRIVATE src/sync/udp_clock.mm)
  target_link_libraries(nocturne_udp_clock PUBLIC "-framework Network" objc)
else()
  target_sources(nocturne_udp_clock PRIVATE src/sync/udp_clock_sockets.cpp)
endif()

# Finding engines on the network: Bonjour on Apple, the DNS-SD API on Windows.
add_library(nocturne_discovery STATIC)
target_include_directories(nocturne_discovery PUBLIC src)
if(APPLE)
  target_sources(nocturne_discovery PRIVATE src/sync/discovery.mm)
  target_link_libraries(nocturne_discovery PUBLIC "-framework Foundation")
elseif(WIN32)
  target_sources(nocturne_discovery PRIVATE src/sync/discovery_win.cpp)
  target_link_libraries(nocturne_discovery PUBLIC nocturne_protocol dnsapi)
endif()

# Blocking HTTP(S) for uploads: NSURLSession on Apple, WinHTTP on Windows.
add_library(nocturne_http STATIC)
target_include_directories(nocturne_http PUBLIC src)
if(APPLE)
  target_sources(nocturne_http PRIVATE src/upload/http.mm)
  target_link_libraries(nocturne_http PUBLIC "-framework Foundation")
elseif(WIN32)
  target_sources(nocturne_http PRIVATE src/upload/http_win.cpp)
  target_link_libraries(nocturne_http PUBLIC winhttp)
endif()

add_executable(nocturne-supervisor src/app/supervisor.cpp)
target_link_libraries(nocturne-supervisor PRIVATE nocturne_protocol)

file(READ src/desk/page.html desk_page)
configure_file(src/desk/page.cpp.in desk_page.cpp @ONLY)
add_library(nocturne_desk STATIC src/desk/desk_server.cpp ${CMAKE_CURRENT_BINARY_DIR}/desk_page.cpp)
target_link_libraries(nocturne_desk PUBLIC nocturne_net)
add_executable(nocturne-desk src/app/desk.cpp)
target_link_libraries(nocturne-desk PRIVATE nocturne_desk)

add_library(nocturne_upload STATIC src/upload/media_sync.cpp src/upload/media_upload.cpp src/upload/s3.cpp
  src/upload/upload_states.cpp)
target_link_libraries(nocturne_upload PUBLIC nocturne_media nocturne_http)

add_library(nocturne_fetch STATIC src/upload/media_fetch.cpp)
target_link_libraries(nocturne_fetch PUBLIC nocturne_upload)

add_library(nocturne_venues STATIC src/venues/venues.cpp src/venues/venue_monitor.cpp src/venues/gig.cpp)
target_link_libraries(nocturne_venues PUBLIC nocturne_net nocturne_discovery nocturne_http)

add_executable(nocturne-ctl src/app/ctl.cpp)
target_link_libraries(nocturne-ctl PRIVATE nocturne_net nocturne_discovery nocturne_venues nocturne_fetch nocturne_audio)

# The UI's engine launcher: it starts, finds and stops the engine beside the UI.
add_library(nocturne_engine_launcher STATIC src/ui/client/engine_launcher.cpp)
target_link_libraries(nocturne_engine_launcher PUBLIC nocturne_protocol)

# The engine stand-in tests/supervisor_process_test.cpp runs under nocturne-supervisor.
add_executable(nocturne_fake_engine tests/fake_engine.cpp)
target_link_libraries(nocturne_fake_engine PRIVATE nocturne_protocol)

# Recording's settings, file naming, disk reserve, rollover and audio timing. Its encoder and return video's H.264
# encoder and decoder are VideoToolbox and AVFoundation on Apple (added below), FFmpeg elsewhere (ADR 0016).
add_library(nocturne_record STATIC src/output/recording.cpp)
target_include_directories(nocturne_record PUBLIC src)
if(NOT APPLE)
  target_sources(nocturne_record PRIVATE src/output/movie_encoder_ffmpeg.cpp)
  target_sources(nocturne_return_video PRIVATE src/output/h264_encoder_ffmpeg.cpp src/output/h264_decoder_ffmpeg.cpp)
  foreach(target nocturne_record nocturne_return_video)
    target_include_directories(${target} PUBLIC ${FFMPEG_INCLUDE_DIRS})
    target_link_directories(${target} PUBLIC ${FFMPEG_LIBRARY_DIRS})
    target_link_libraries(${target} PUBLIC ${FFMPEG_LIBRARIES})
  endforeach()
endif()

enable_testing()
# The engine suite. Its portable cases build everywhere; the Apple section below adds the rest.
add_executable(nocturne_tests tests/main.cpp tests/atomic_write_test.cpp tests/autopilot_stepper_test.cpp
  tests/isf_test.cpp tests/machine_actions_test.cpp tests/audio_test.cpp tests/audio_fft_test.cpp
  tests/audio_input_test.cpp tests/opus_codec_test.cpp tests/hap_codec_test.cpp tests/hap_test.cpp
  tests/media_files_test.cpp tests/video_decoder_test.cpp
  tests/websocket_frames_test.cpp tests/websocket_test.cpp
  tests/udp_clock_test.cpp tests/http_test.cpp tests/crash_guard_test.cpp tests/supervisor_process_test.cpp
  tests/bundle_update_test.cpp tests/h264_decoder_test.cpp
  ${NOCTURNE_PLATFORM_TESTS})
target_include_directories(nocturne_tests PRIVATE tests third_party/doctest)
target_compile_definitions(nocturne_tests PRIVATE NOCTURNE_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden"
  NOCTURNE_MEDIA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/media"
  NOCTURNE_PLATFORM_CHILD="$<TARGET_FILE:nocturne_platform_child>"
  NOCTURNE_FAKE_ENGINE="$<TARGET_FILE:nocturne_fake_engine>" NOCTURNE_SUPERVISOR="$<TARGET_FILE:nocturne-supervisor>"
  NOCTURNE_DESK="$<TARGET_FILE:nocturne-desk>")
add_dependencies(nocturne_tests nocturne_platform_child nocturne_fake_engine nocturne-supervisor nocturne-desk)
target_link_libraries(nocturne_tests PRIVATE nocturne_audio)
if(APPLE)
  target_sources(nocturne_tests PRIVATE src/audio/power_spectrum_pocketfft.cpp)
  target_include_directories(nocturne_tests PRIVATE "${POCKETFFT_INCLUDE_DIR}")
endif()
add_test(NAME nocturne_tests COMMAND nocturne_tests)

add_executable(nocturne-hap-encode src/app/hap_encode.cpp)
target_link_libraries(nocturne-hap-encode PRIVATE nocturne_media)

# The JUCE UI. On Apple it also links targets defined below this line (local previews, the harness).
add_subdirectory(src/ui)

# Everything below needs Apple frameworks, Objective-C++ or metal-cpp. A target moves above this line once its other
# platforms' sources exist (ADR 0016). On Apple the harness links the portable libraries into the tests.
if(NOT APPLE)
  target_sources(nocturne_tests PRIVATE tests/discovery_test.cpp tests/venues_key_test.cpp)
  target_link_libraries(nocturne_tests PRIVATE nocturne_engine_logic nocturne_shader_cross nocturne_media nocturne_net
    nocturne_udp_clock nocturne_discovery nocturne_http nocturne_venues)
  return()
endif()

target_sources(nocturne_return_video PRIVATE src/output/h264_stream.mm)
target_link_libraries(nocturne_return_video PUBLIC "-framework VideoToolbox" "-framework CoreMedia"
  "-framework CoreVideo" "-framework Foundation")

add_library(nocturne_engine STATIC
  src/engine/effect_stepper.cpp
  src/engine/chain_compiler.cpp
  src/engine/clip_effects_preview.cpp
  src/engine/clock_service.cpp
  src/engine/cue_service.cpp
  src/engine/desk_service.cpp
  src/engine/engine.cpp
  src/engine/envelope.cpp
  src/engine/auto_range.cpp
  src/engine/frame_guard.cpp
  src/engine/frame_ring.cpp
  src/engine/frame_trace.cpp
  src/engine/bundle_update_apple.cpp
  src/engine/fsr_upscaler.cpp
  src/engine/machine_service.cpp
  src/engine/machine_settings.cpp
  src/engine/metal_impl.cpp
  src/engine/metal_util.cpp
  src/engine/modulation_plan.cpp
  src/engine/modulation_service.cpp
  src/engine/playhead.cpp
  src/engine/preview_service.cpp
  src/engine/render_thread.cpp
  src/engine/screen_painter.cpp
  src/engine/settings_service.cpp
  src/engine/side_targets.cpp
  src/engine/static_bake.cpp
  src/engine/switch_log.cpp
  src/engine/gpu_profile.cpp
  src/engine/isf_library.cpp
  src/engine/isf_service.cpp
  src/engine/trigger_service.cpp
  src/engine/wall_cue_service.cpp
  src/output/display_output.cpp
  src/output/display_keeper.cpp
  src/output/known_displays.cpp
  src/output/output_service.cpp
  src/output/outputs.cpp
  src/output/record_service.cpp
  src/output/screen_displays.cpp
  src/output/recorder.mm
  src/output/return_video_service.cpp
  src/output/ycbcr_converter.mm
  src/platform/mac_displays.mm
  src/platform/thermal.mm
)
if(NOCTURNE_TRACY)
  target_sources(nocturne_engine PRIVATE src/platform/profile_metal.mm)
endif()
target_include_directories(nocturne_engine PUBLIC src third_party/metal-cpp)
target_link_libraries(nocturne_engine PUBLIC
  nocturne_engine_logic
  nocturne_media
  nocturne_record
  nocturne_return_video
  nocturne_audio
  nocturne_shader_cross
  "-framework Metal"
  "-framework QuartzCore"
  "-framework IOSurface"
  "-framework VideoToolbox"
  "-framework Foundation"
  "-framework AppKit"
  "-framework IOKit"
  "-framework Security"
)

add_library(nocturne_djlink STATIC src/djlink/clock_follower.cpp src/djlink/djlink_packets.cpp src/djlink/djlink_receiver.cpp
  src/djlink/djlink_service.cpp src/djlink/djlink_state.cpp)
target_link_libraries(nocturne_djlink PUBLIC nocturne_engine)

add_library(nocturne_import STATIC src/import/resolume_import.cpp)
target_link_libraries(nocturne_import PUBLIC nocturne_media)

add_executable(nocturne-import src/app/import.cpp)
target_link_libraries(nocturne-import PRIVATE nocturne_import)

add_executable(nocturne-upload src/app/upload.cpp)
target_link_libraries(nocturne-upload PRIVATE nocturne_upload)

add_executable(nocturne-decode-bench src/app/decode_bench.mm)
target_link_libraries(nocturne-decode-bench PRIVATE nocturne_media)

add_library(nocturne_harness STATIC src/harness/engine_harness.cpp src/harness/fake_audio_input.cpp
  src/harness/fake_displays.cpp)
target_link_libraries(nocturne_harness PUBLIC nocturne_engine)

add_library(nocturne_sync STATIC src/sync/node_sync.mm)
target_link_libraries(nocturne_sync PUBLIC nocturne_engine nocturne_net nocturne_discovery nocturne_udp_clock)

add_library(nocturne_app_config STATIC src/app/engine_config.cpp)
target_link_libraries(nocturne_app_config PUBLIC nocturne_engine nocturne_net)

add_executable(nocturne src/app/main.mm src/app/safe_mode.mm)
target_link_libraries(nocturne PRIVATE nocturne_app_config nocturne_harness nocturne_audio nocturne_net nocturne_sync nocturne_djlink nocturne_upload)

set(NOCTURNE_APP "${CMAKE_BINARY_DIR}/Nocturne.app")
set_target_properties(nocturne nocturne-supervisor nocturne-ctl nocturne-desk PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${NOCTURNE_APP}/Contents/MacOS")
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/Nocturne.app.signed"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/src/app/Info.plist" "${NOCTURNE_APP}/Contents/Info.plist"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/tools/update-bundle.sh"
    "${NOCTURNE_APP}/Contents/Resources/update-bundle.sh"
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/sign-bundle.sh" "${NOCTURNE_APP}"
  COMMAND ${CMAKE_COMMAND} -E create_symlink Nocturne.app/Contents/MacOS/nocturne "${CMAKE_BINARY_DIR}/nocturne"
  COMMAND ${CMAKE_COMMAND} -E create_symlink Nocturne.app/Contents/MacOS/nocturne-supervisor "${CMAKE_BINARY_DIR}/nocturne-supervisor"
  COMMAND ${CMAKE_COMMAND} -E create_symlink Nocturne.app/Contents/MacOS/nocturne-ctl "${CMAKE_BINARY_DIR}/nocturne-ctl"
  COMMAND ${CMAKE_COMMAND} -E create_symlink Nocturne.app/Contents/MacOS/nocturne-desk "${CMAKE_BINARY_DIR}/nocturne-desk"
  COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_BINARY_DIR}/Nocturne.app.signed"
  DEPENDS nocturne nocturne-supervisor nocturne-ctl nocturne-desk src/app/Info.plist tools/sign-bundle.sh tools/update-bundle.sh
  VERBATIM)
add_custom_target(app ALL DEPENDS "${CMAKE_BINARY_DIR}/Nocturne.app.signed")

target_sources(nocturne_tests PRIVATE tests/harness_test.cpp tests/golden.cpp tests/protocol_test.cpp
  tests/media_test.cpp tests/effects_test.cpp tests/transport_test.cpp tests/layers_test.cpp
  tests/jump_reverse_test.cpp tests/trigger_test.cpp tests/screens_test.cpp tests/autopilot_test.cpp tests/column_autopilot_test.cpp
  tests/transition_test.cpp tests/audio_service_test.cpp tests/supervisor_test.cpp
  tests/modulation_test.cpp tests/composition_test.cpp tests/import_test.cpp tests/output_import_test.cpp
  tests/clip_effects_test.cpp tests/classic_test.cpp tests/classic_stripper_test.cpp tests/deck_layers_test.cpp
  tests/mapping_test.cpp tests/cue_test.cpp tests/text_test.cpp tests/preview_test.cpp tests/sync_test.cpp
  tests/generator_test.cpp tests/prototype_gaps_test.cpp tests/continuity_test.cpp
  tests/record_test.mm tests/resilience_test.cpp tests/still_test.cpp tests/colour_test.cpp tests/engine_config_test.cpp
  tests/playhead_test.cpp tests/upload_test.cpp tests/profile_test.cpp
  tests/djlink_test.cpp tests/effect_steps_test.cpp tests/venues_test.cpp tests/venue_alerts_test.cpp tests/machine_test.cpp tests/machine_settings_test.cpp tests/static_bake_test.cpp tests/settings_test.cpp tests/gig_test.cpp tests/switch_log_test.cpp tests/desk_test.mm tests/palette_steps_test.cpp
  tests/update_steps_test.cpp tests/media_sync_test.cpp tests/upscale_test.cpp tests/render_scale_test.cpp tests/isf_engine_test.cpp tests/isf_user_test.cpp tests/prepare_test.cpp
  tests/return_video_test.cpp tests/return_audio_test.cpp tests/rename_test.cpp tests/pairing_test.cpp tests/remote_files_test.cpp
  tests/hap_playback_test.cpp
  tests/hap_transcode_test.cpp tests/hap_golden_test.cpp tests/hardening_files_test.cpp)
target_compile_definitions(nocturne_tests PRIVATE NOCTURNE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  NOCTURNE_HAP_ENCODE="$<TARGET_FILE:nocturne-hap-encode>")
add_dependencies(nocturne_tests app nocturne-hap-encode)
target_link_libraries(nocturne_tests PRIVATE nocturne_app_config nocturne_harness nocturne_audio nocturne_net nocturne_sync nocturne_import nocturne_upload nocturne_djlink nocturne_venues "-framework CoreGraphics" "-framework ImageIO"
  "-framework CoreFoundation")
add_test(NAME mini_host_test COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/mini_host_test.sh")
target_sources(nocturne_tests PRIVATE tests/keep_local_test.cpp)
target_link_libraries(nocturne_tests PRIVATE nocturne_fetch nocturne_desk "-framework WebKit")

# The protocol, pairing and clock cases again, against the plain-socket WebSocket and UDP clock other platforms use
# instead of Network.framework. The product keeps nocturne_net and nocturne_udp_clock.
add_library(nocturne_net_sockets STATIC src/net/websocket_frames.cpp src/net/websocket_rules.cpp
  src/net/websocket_sockets.cpp)
target_link_libraries(nocturne_net_sockets PUBLIC nocturne_protocol nocturne_return_video)
add_library(nocturne_udp_clock_sockets STATIC src/sync/clock_estimator.cpp src/sync/udp_clock_sockets.cpp)
target_link_libraries(nocturne_udp_clock_sockets PUBLIC nocturne_protocol)
add_executable(nocturne_socket_tests tests/main.cpp tests/golden.cpp tests/protocol_test.cpp tests/pairing_test.cpp
  tests/websocket_test.cpp tests/udp_clock_test.cpp)
target_include_directories(nocturne_socket_tests PRIVATE tests third_party/doctest)
get_target_property(test_definitions nocturne_tests COMPILE_DEFINITIONS)
target_compile_definitions(nocturne_socket_tests PRIVATE ${test_definitions})
add_dependencies(nocturne_socket_tests app)
target_link_libraries(nocturne_socket_tests PRIVATE nocturne_harness nocturne_net_sockets nocturne_udp_clock_sockets
  "-framework CoreGraphics" "-framework ImageIO" "-framework CoreFoundation")
add_test(NAME nocturne_socket_tests COMMAND nocturne_socket_tests)

option(NOCTURNE_FUZZ "Add the target fuzz: the libFuzzer harnesses in fuzz/, built with Homebrew LLVM" OFF)
if(NOCTURNE_FUZZ)
  add_custom_target(fuzz COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/build.sh" "${CMAKE_BINARY_DIR}" USES_TERMINAL)
endif()
