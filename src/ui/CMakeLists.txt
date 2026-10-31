# The JUCE UI (ADR 0002): its own process; "Nocturne UI.app" on macOS, "Nocturne UI.exe" beside nocturne.exe on
# Windows. JUCE does not build under ARC, so this directory drops the parent's -fobjc-arc.
if(MSVC)
  set_property(DIRECTORY PROPERTY COMPILE_OPTIONS /W4 -Wno-unused-parameter /utf-8)
else()
  set_property(DIRECTORY PROPERTY COMPILE_OPTIONS -Wall -Wno-unused-parameter)
endif()

enable_language(C)
if(APPLE)
  enable_language(OBJC)
endif()
include(FetchContent)
FetchContent_Declare(juce
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 8.0.9
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(juce)

add_library(nocturne_ui_client STATIC client/engine_client.cpp client/connection.cpp client/return_video_client.cpp
  client/return_audio_player.cpp)
target_include_directories(nocturne_ui_client PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(nocturne_ui_client PUBLIC nocturne_net nocturne_venues nocturne_audio nocturne_engine_launcher)
if(APPLE)
  target_sources(nocturne_ui_client PRIVATE client/return_audio_output_apple.cpp)
endif()

# Local previews run a follower engine in this process, so they need the engine's GPU path; elsewhere the engine
# paints them.
add_library(nocturne_ui_local STATIC client/director_media.cpp)
target_link_libraries(nocturne_ui_local PUBLIC nocturne_ui_client nocturne_fetch)
if(APPLE)
  target_sources(nocturne_ui_local PRIVATE client/local_previews.cpp)
  target_link_libraries(nocturne_ui_local PUBLIC nocturne_sync)
else()
  target_sources(nocturne_ui_local PRIVATE client/local_previews_none.cpp)
endif()

set(NOCTURNE_UI_SOURCES
  app/alert_strip.cpp
  app/bottom_panes.cpp
  app/chain_pane.cpp
  app/step_strip.cpp
  app/clip_grid.cpp
  app/colour_picker.cpp
  app/cues_view.cpp
  app/gig_editor.cpp
  app/look.cpp
  app/machines_view.cpp
  app/macro_pane.cpp
  app/main_component.cpp
  app/map_canvas.cpp
  app/map_geometry.cpp
  app/map_inspector.cpp
  app/map_view.cpp
  app/mid_tabs.cpp
  app/midi.cpp
  app/midi_settings.cpp
  app/monitors.cpp
  app/params.cpp
  app/preset_tree.cpp
  app/session.cpp
  app/settings_modal.cpp
  app/show_browser.cpp
  app/tooltips.cpp
  app/transport_bar.cpp
  app/venue_menu.cpp
  app/widgets.cpp)

# JUCE's module sources compile once, into this library; the app and the tests link it.
add_library(nocturne_ui_components STATIC ${NOCTURNE_UI_SOURCES})
target_link_libraries(nocturne_ui_components
  PUBLIC nocturne_ui_client nocturne_ui_local
  PRIVATE juce::juce_gui_basics juce::juce_audio_devices juce::juce_recommended_config_flags)
target_compile_definitions(nocturne_ui_components
  PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_STANDALONE_APPLICATION=1 JUCE_MODAL_LOOPS_PERMITTED=1
  INTERFACE $<TARGET_PROPERTY:nocturne_ui_components,COMPILE_DEFINITIONS>)
target_include_directories(nocturne_ui_components INTERFACE $<TARGET_PROPERTY:nocturne_ui_components,INCLUDE_DIRECTORIES>)
if(NOT APPLE)
  # Return audio plays through JUCE where there is no AudioQueue; JUCE's modules compile into this library.
  target_sources(nocturne_ui_components PRIVATE client/return_audio_output_juce.cpp)
endif()

# The Windows executable carries the app icon; the macOS bundle keeps the look it has.
set(ui_icon)
if(WIN32)
  set(ui_icon ICON_BIG "${CMAKE_SOURCE_DIR}/assets/icon/nocturne.png")
endif()
juce_add_gui_app(nocturne-ui
  PRODUCT_NAME "Nocturne UI"
  BUNDLE_ID com.azylume.nocturne.ui
  COMPANY_NAME Azylume
  VERSION 0.1.1
  ${ui_icon}
  PLIST_TO_MERGE "<plist><dict><key>LSUIElement</key><true/><key>NSLocalNetworkUsageDescription</key><string>Nocturne UI connects to Nocturne engines on the local network.</string><key>NSBonjourServices</key><array><string>_nocturne._tcp</string></array><key>LSMinimumSystemVersion</key><string>14.0</string></dict></plist>")
target_sources(nocturne-ui PRIVATE app/main.cpp)
target_link_libraries(nocturne-ui PRIVATE nocturne_ui_components juce::juce_recommended_config_flags)
set_target_properties(nocturne-ui PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if(APPLE)
  set(NOCTURNE_UI_APP "${CMAKE_BINARY_DIR}/Nocturne UI.app")
  add_custom_command(TARGET nocturne-ui POST_BUILD
    COMMAND "${CMAKE_SOURCE_DIR}/tools/sign-bundle.sh" --ui "${NOCTURNE_UI_APP}"
    VERBATIM)
endif()

# The UI suite. Its cases without an engine build everywhere; the ones that drive the in-process engine
# (EngineHarness) need its GPU path, so they build where it does.
add_executable(nocturne_ui_tests ${CMAKE_SOURCE_DIR}/tests/ui/main.cpp ${CMAKE_SOURCE_DIR}/tests/ui/params_test.cpp
  ${CMAKE_SOURCE_DIR}/tests/ui/map_geometry_test.cpp ${CMAKE_SOURCE_DIR}/tests/ui/engine_launcher_test.cpp
  ${CMAKE_SOURCE_DIR}/tests/ui/return_audio_test.cpp)
target_include_directories(nocturne_ui_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests ${CMAKE_SOURCE_DIR}/third_party/doctest)
target_compile_definitions(nocturne_ui_tests PRIVATE NOCTURNE_MEDIA_DIR="${CMAKE_SOURCE_DIR}/tests/media"
  NOCTURNE_UI_SHOTS="${CMAKE_BINARY_DIR}/ui-shots" NOCTURNE_FAKE_ENGINE="$<TARGET_FILE:nocturne_fake_engine>")
add_dependencies(nocturne_ui_tests nocturne_fake_engine)
target_link_libraries(nocturne_ui_tests PRIVATE nocturne_ui_components nocturne_upload juce::juce_recommended_config_flags)
if(APPLE)
  target_sources(nocturne_ui_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/ui/client_test.cpp
    ${CMAKE_SOURCE_DIR}/tests/ui/smoke_test.cpp ${CMAKE_SOURCE_DIR}/tests/ui/panels_test.cpp
    ${CMAKE_SOURCE_DIR}/tests/ui/map_test.cpp ${CMAKE_SOURCE_DIR}/tests/ui/monitor_handles_test.cpp
    ${CMAKE_SOURCE_DIR}/tests/ui/parity_test.cpp ${CMAKE_SOURCE_DIR}/tests/ui/tooltip_test.cpp
    ${CMAKE_SOURCE_DIR}/tests/ui/local_previews_test.cpp ${CMAKE_SOURCE_DIR}/tests/ui/scroll_test.cpp
    ${CMAKE_SOURCE_DIR}/tests/ui/cues_test.cpp ${CMAKE_SOURCE_DIR}/tests/ui/preview_noise_test.cpp
    ${CMAKE_SOURCE_DIR}/tests/ui/venue_test.cpp ${CMAKE_SOURCE_DIR}/tests/ui/machines_test.cpp
    ${CMAKE_SOURCE_DIR}/tests/ui/prepare_test.cpp
    ${CMAKE_SOURCE_DIR}/tests/ui/listen_test.cpp ${CMAKE_SOURCE_DIR}/tests/ui/midi_test.cpp)
  target_link_libraries(nocturne_ui_tests PRIVATE nocturne_harness)
endif()
add_test(NAME nocturne_ui_tests COMMAND nocturne_ui_tests)
