cmake_minimum_required(VERSION 3.24)

set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "")

project(nocturne VERSION 0.1.1 LANGUAGES CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)

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

add_library(nocturne_shader_cross STATIC src/engine/shader_cross.cpp)
target_include_directories(nocturne_shader_cross PUBLIC src)
target_link_libraries(nocturne_shader_cross PRIVATE glslang::glslang glslang::glslang-default-resource-limits
  spirv-cross-msl)

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
target_include_directories(nocturne_protocol SYSTEM PUBLIC third_party/nlohmann)

add_library(nocturne_net STATIC src/net/websocket.mm)
target_link_libraries(nocturne_net PUBLIC nocturne_protocol nocturne_return_video "-framework Network" objc)

add_library(nocturne_return_video STATIC src/output/return_video.cpp src/output/h264_stream.mm)
target_link_libraries(nocturne_return_video PUBLIC nocturne_protocol "-framework VideoToolbox" "-framework CoreMedia"
  "-framework CoreVideo" "-framework Foundation")

add_library(nocturne_media STATIC
  src/media/clip_decoders.cpp
  src/media/composition_media.cpp
  src/media/composition_service.cpp
  src/media/hap.cpp
  src/media/hap_encoder.cpp
  src/media/hap_movie.mm
  src/media/hap_texture.mm
  src/media/keep_local.cpp
  src/media/library_service.cpp
  src/media/media_library.cpp
  src/media/snappy.cpp
  src/media/stills.mm
  src/media/text_renderer.cpp
  src/media/video_decoder.mm
)
target_include_directories(nocturne_media PUBLIC src third_party/metal-cpp)
target_link_libraries(nocturne_media PUBLIC
  nocturne_protocol
  nocturne_profile
  "-framework AVFoundation"
  "-framework CoreMedia"
  "-framework CoreVideo"
  "-framework CoreText"
  "-framework CoreGraphics"
  "-framework ImageIO"
  "-framework Metal"
  "-framework Foundation"
)

add_library(nocturne_engine STATIC
  src/engine/autopilot_stepper.cpp
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
  src/engine/bundle_update.cpp
  src/engine/fsr_upscaler.cpp
  src/engine/machine_actions.cpp
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
  src/engine/isf_compiler.cpp
  src/engine/isf_library.cpp
  src/engine/isf_service.cpp
  src/engine/transport.cpp
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
  nocturne_media
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

add_library(nocturne_audio STATIC
  src/audio/audio_analysis.cpp
  src/audio/audio_analyzer.cpp
  src/audio/audio_device_settings.cpp
  src/audio/audio_input.mm
  src/audio/audio_service.cpp
  src/audio/opus_codec.cpp
  src/audio/return_audio.cpp
)
target_include_directories(nocturne_audio PUBLIC src)
target_link_libraries(nocturne_audio PUBLIC
  nocturne_protocol
  "-framework Accelerate"
  "-framework AudioToolbox"
  "-framework AVFoundation"
  "-framework CoreAudio"
  "-framework Foundation"
)

add_library(nocturne_djlink STATIC src/djlink/clock_follower.cpp src/djlink/djlink_packets.cpp src/djlink/djlink_receiver.cpp
  src/djlink/djlink_service.cpp src/djlink/djlink_state.cpp)
target_link_libraries(nocturne_djlink PUBLIC nocturne_engine)

add_library(nocturne_import STATIC src/import/resolume_import.cpp)
target_link_libraries(nocturne_import PUBLIC nocturne_media)

add_executable(nocturne-import src/app/import.cpp)
target_link_libraries(nocturne-import PRIVATE nocturne_import)

add_library(nocturne_http STATIC src/upload/http.mm)
target_include_directories(nocturne_http PUBLIC src)
target_link_libraries(nocturne_http PUBLIC "-framework Foundation")

add_library(nocturne_upload STATIC src/upload/media_sync.cpp src/upload/media_upload.cpp src/upload/s3.cpp
  src/upload/upload_states.cpp)
target_link_libraries(nocturne_upload PUBLIC nocturne_media nocturne_http)

add_executable(nocturne-upload src/app/upload.cpp)
target_link_libraries(nocturne-upload PRIVATE nocturne_upload)

add_executable(nocturne-hap-encode src/app/hap_encode.cpp)
target_link_libraries(nocturne-hap-encode PRIVATE nocturne_media)

add_executable(nocturne-decode-bench src/app/decode_bench.mm)
target_link_libraries(nocturne-decode-bench PRIVATE nocturne_media)

add_library(nocturne_fetch STATIC src/upload/media_fetch.cpp)
target_link_libraries(nocturne_fetch PUBLIC nocturne_upload)

add_library(nocturne_harness STATIC src/harness/engine_harness.cpp src/harness/fake_audio_input.cpp
  src/harness/fake_displays.cpp)
target_link_libraries(nocturne_harness PUBLIC nocturne_engine)

add_library(nocturne_discovery STATIC src/sync/discovery.mm)
target_include_directories(nocturne_discovery PUBLIC src)
target_link_libraries(nocturne_discovery PUBLIC "-framework Foundation")

add_library(nocturne_venues STATIC src/venues/venues.cpp src/venues/venue_monitor.cpp src/venues/gig.cpp)
target_link_libraries(nocturne_venues PUBLIC nocturne_net nocturne_discovery nocturne_http)

add_library(nocturne_sync STATIC src/sync/clock_estimator.cpp src/sync/node_sync.mm src/sync/udp_clock.mm)
target_link_libraries(nocturne_sync PUBLIC nocturne_engine nocturne_net nocturne_discovery "-framework Network")

add_library(nocturne_app_config STATIC src/app/engine_config.cpp)
target_link_libraries(nocturne_app_config PUBLIC nocturne_engine nocturne_net)

add_executable(nocturne src/app/main.mm src/app/safe_mode.mm)
target_link_libraries(nocturne PRIVATE nocturne_app_config nocturne_harness nocturne_audio nocturne_net nocturne_sync nocturne_djlink nocturne_upload)

add_executable(nocturne-supervisor src/app/supervisor.cpp)
target_link_libraries(nocturne-supervisor PRIVATE nocturne_protocol)

file(READ src/desk/page.html desk_page)
configure_file(src/desk/page.cpp.in desk_page.cpp @ONLY)
add_library(nocturne_desk STATIC src/desk/desk_server.cpp ${CMAKE_CURRENT_BINARY_DIR}/desk_page.cpp)
target_link_libraries(nocturne_desk PUBLIC nocturne_net)
add_executable(nocturne-desk src/app/desk.cpp)
target_link_libraries(nocturne-desk PRIVATE nocturne_desk)

add_executable(nocturne-ctl src/app/ctl.cpp)
target_link_libraries(nocturne-ctl PRIVATE nocturne_net nocturne_discovery nocturne_venues nocturne_fetch nocturne_audio)

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

enable_testing()
add_executable(nocturne_tests tests/main.cpp tests/harness_test.cpp tests/golden.cpp tests/protocol_test.cpp
  tests/media_test.cpp tests/effects_test.cpp tests/transport_test.cpp tests/layers_test.cpp
  tests/jump_reverse_test.cpp tests/trigger_test.cpp tests/screens_test.cpp tests/autopilot_test.cpp tests/column_autopilot_test.cpp
  tests/transition_test.cpp tests/audio_test.cpp tests/audio_service_test.cpp tests/supervisor_test.cpp
  tests/modulation_test.cpp tests/composition_test.cpp tests/import_test.cpp tests/output_import_test.cpp
  tests/clip_effects_test.cpp tests/classic_test.cpp tests/classic_stripper_test.cpp tests/deck_layers_test.cpp
  tests/mapping_test.cpp tests/cue_test.cpp tests/text_test.cpp tests/preview_test.cpp tests/sync_test.cpp
  tests/generator_test.cpp tests/prototype_gaps_test.cpp tests/continuity_test.cpp
  tests/record_test.mm tests/resilience_test.cpp tests/still_test.cpp tests/colour_test.cpp tests/engine_config_test.cpp
  tests/autopilot_stepper_test.cpp tests/playhead_test.cpp tests/upload_test.cpp tests/profile_test.cpp
  tests/djlink_test.cpp tests/effect_steps_test.cpp tests/venues_test.cpp tests/venue_alerts_test.cpp tests/machine_test.cpp tests/machine_settings_test.cpp tests/static_bake_test.cpp tests/settings_test.cpp tests/gig_test.cpp tests/switch_log_test.cpp tests/desk_test.mm tests/palette_steps_test.cpp
  tests/update_steps_test.cpp tests/media_sync_test.cpp tests/upscale_test.cpp tests/render_scale_test.cpp tests/isf_test.cpp tests/isf_engine_test.cpp tests/isf_user_test.cpp tests/prepare_test.cpp
  tests/return_video_test.cpp tests/return_audio_test.cpp tests/rename_test.cpp tests/pairing_test.cpp tests/remote_files_test.cpp
  tests/hap_test.cpp tests/hap_codec_test.cpp tests/hap_playback_test.cpp
  tests/hap_transcode_test.cpp tests/hap_golden_test.cpp tests/hardening_files_test.cpp)
target_include_directories(nocturne_tests PRIVATE tests third_party/doctest)
target_compile_definitions(nocturne_tests PRIVATE NOCTURNE_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden"
  NOCTURNE_MEDIA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/media" NOCTURNE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  NOCTURNE_SUPERVISOR="$<TARGET_FILE:nocturne-supervisor>"
  NOCTURNE_HAP_ENCODE="$<TARGET_FILE:nocturne-hap-encode>")
add_dependencies(nocturne_tests app nocturne-hap-encode)
target_link_libraries(nocturne_tests PRIVATE nocturne_app_config nocturne_harness nocturne_audio nocturne_net nocturne_sync nocturne_import nocturne_upload nocturne_djlink nocturne_venues "-framework CoreGraphics" "-framework ImageIO"
  "-framework CoreFoundation")
add_test(NAME nocturne_tests COMMAND nocturne_tests)
add_test(NAME mini_host_test COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/mini_host_test.sh")
target_sources(nocturne_tests PRIVATE tests/keep_local_test.cpp)
target_link_libraries(nocturne_tests PRIVATE nocturne_fetch nocturne_desk "-framework WebKit")

option(NOCTURNE_FUZZ "Add the target fuzz: the libFuzzer harnesses in fuzz/, built with Homebrew LLVM" OFF)
if(NOCTURNE_FUZZ)
  add_custom_target(fuzz COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/build.sh" "${CMAKE_BINARY_DIR}" USES_TERMINAL)
endif()

add_subdirectory(src/ui)
