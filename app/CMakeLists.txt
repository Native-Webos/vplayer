# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.22)
project(vplayer LANGUAGES C CXX)

# The kit: -DWNK_KIT_DIR=..., else $WNK_KIT_DIR, else where it was when tools/new-app.py made this.
# A relative path is taken from this folder (a git submodule at kit/ would be "kit").
if (DEFINED ENV{WNK_KIT_DIR})
    set(_kit_default "$ENV{WNK_KIT_DIR}")
else ()
    set(_kit_default "../../native-webos/webos-native-app-kit")
endif ()
set(WNK_KIT_DIR "${_kit_default}" CACHE PATH "The webOS native app kit (absolute, or relative to this folder)")
get_filename_component(_kit "${WNK_KIT_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
if (NOT EXISTS "${_kit}/cmake/WnkApp.cmake")
    message(FATAL_ERROR "The webOS native app kit is not at ${_kit}: set WNK_KIT_DIR")
endif ()
# set(WNK_FFMPEG ON)  # media playback: see the kit's docs/AGENT-APP-GUIDE.md, "Media playback"
include("${_kit}/cmake/WnkApp.cmake")

add_executable(vplayer main.cpp vlc_engine.cpp library.cpp pages.cpp ui/theme.cpp network.cpp network_page.cpp vlc_dialogs.cpp history.cpp app_settings.cpp playlist.cpp playlists_page.cpp music.cpp music_page.cpp search.cpp thumbnails.cpp
        player/frame_sink.cpp player/vlc_playback.cpp player/vlc_vars.cpp player/demo_playback.cpp player/video_view.cpp
        player/player_screen.cpp player/vlc_source.cpp player/hw_playback.cpp player/subtitle_text.cpp
        player/subtitle_bitmap.cpp player/dovi_detect.cpp
        player/ebml.cpp player/mkv_attachments.cpp player/teletext.cpp player/cea608.cpp)
# ../modules: the wnk_bridge contract (wnk_bridge/wnk_bridge.h) shared with our VLC stream output.
target_include_directories(vplayer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../modules")
# The kit's playback controller and the TV's decoders (Starfish, NDL; dlopened, never DT_NEEDED).
target_link_libraries(vplayer PRIVATE wnk_app wnk_media wnk_media_ui wnk_media_starfish wnk_media_ndl)
target_include_directories(vplayer SYSTEM PRIVATE "${WNK_KIT_DIR}/third_party")  # nlohmann/json.hpp

# libVLC, cross-built by ../libvlc/build.sh: headers in its stage, the stripped runtime (libvlc,
# libvlccore, plugins, lua) laid out as it goes into the package. The host build has no libVLC yet
# and compiles VlcEngine as a stub.
set(VPLAYER_LIBVLC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build/libvlc" CACHE PATH "libVLC build output (libvlc/build.sh)")
set(_vlc_runtime "${VPLAYER_LIBVLC_DIR}/runtime")
set(_vlc_include "${VPLAYER_LIBVLC_DIR}/stage/usr/include")
set(VPLAYER_LIBVLC_PACKAGE_DIRS)
if (WNK_WEBOS AND EXISTS "${_vlc_runtime}/lib/libvlc.so.5" AND EXISTS "${_vlc_include}/vlc/vlc.h")
    message(STATUS "vplayer: libVLC from ${VPLAYER_LIBVLC_DIR}")
    target_compile_definitions(vplayer PRIVATE VPLAYER_HAVE_LIBVLC=1)
    target_include_directories(vplayer SYSTEM PRIVATE "${_vlc_include}")
    # VLC's plugin headers (they include one another by bare name), for the one file that needs them.
    set_source_files_properties(player/vlc_vars.cpp PROPERTIES COMPILE_OPTIONS "-isystem;${_vlc_include}/vlc/plugins")
    target_link_libraries(vplayer PRIVATE "${_vlc_runtime}/lib/libvlc.so.5" "${_vlc_runtime}/lib/libvlccore.so.9")
    target_link_options(vplayer PRIVATE "LINKER:-rpath-link,${_vlc_runtime}/lib")  # libvlccore.so.9
    # lib/ (libvlc.so.5, libvlccore.so.9, vlc/plugins, vlc/lua) and share/ (vlc/lua) are merged into
    # the package's own lib/ and share/.
    set(VPLAYER_LIBVLC_PACKAGE_DIRS "${_vlc_runtime}/lib" "${_vlc_runtime}/share")
elseif (WNK_WEBOS)
    message(WARNING "vplayer: no libVLC at ${VPLAYER_LIBVLC_DIR} (run ../libvlc/build.sh); building without it")
endif ()

# Fonts (Inter, OFL: assets/fonts/OFL.txt) and the SVG icons live in fonts/ and icons/ next to the
# executable, on the host and in the ipk.
set(VPLAYER_FONT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts")
set(VPLAYER_ICON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons")
add_custom_command(TARGET vplayer POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${VPLAYER_FONT_DIR}" "$<TARGET_FILE_DIR:vplayer>/fonts"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${VPLAYER_ICON_DIR}" "$<TARGET_FILE_DIR:vplayer>/icons"
        VERBATIM)

wnk_add_webos_package(
        TARGET vplayer
        APPID com.sostk.vplayer
        TITLE "VPlayer"
        VERSION 0.1.0
        VENDOR "sostk"
        ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.png"
        LARGE_ICON "${CMAKE_CURRENT_SOURCE_DIR}/largeIcon.png"
        ICON_COLOR "#e85d04"
        EXPECTED_NEEDED "${CMAKE_CURRENT_SOURCE_DIR}/expected-needed.txt"
        REQUIRED_PERMISSIONS database.operation
        EXTRA_DIRS "${VPLAYER_FONT_DIR}" "${VPLAYER_ICON_DIR}" ${VPLAYER_LIBVLC_PACKAGE_DIRS})

# Host: the player opens (demo engine), shows its first frame and closes with Back.
wnk_add_app_test(NAME e2e.vplayer.player_opens TARGET vplayer PREFIX APP_VPLAYER
        ARGS "{\"play\":\"/nonexistent/Demo.mkv\"}"
        KEYS "wait@1500,Escape,wait@300,Escape,Escape" PASS "first frame shown")

# Host unit tests (the kit's tiny harness): pure parts of the player.
if (NOT WNK_WEBOS)
    add_executable(test_subtitle_text tests/test_subtitle_text.cpp player/subtitle_text.cpp)
    target_include_directories(test_subtitle_text PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${_kit}/tests/unit")
    target_link_libraries(test_subtitle_text PRIVATE wnk_media wnk_media_ui)
    target_compile_definitions(test_subtitle_text PRIVATE VPLAYER_TEST_FONT="${VPLAYER_FONT_DIR}/Inter-SemiBold.ttf")
    add_test(NAME unit.vplayer.subtitle_text COMMAND test_subtitle_text)
    add_executable(test_subtitle_bitmap tests/test_subtitle_bitmap.cpp player/subtitle_bitmap.cpp)
    target_include_directories(test_subtitle_bitmap PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${_kit}/tests/unit")
    target_link_libraries(test_subtitle_bitmap PRIVATE wnk_media)
    add_test(NAME unit.vplayer.subtitle_bitmap COMMAND test_subtitle_bitmap)
    add_executable(test_dovi_detect tests/test_dovi_detect.cpp player/dovi_detect.cpp player/ebml.cpp)
    target_include_directories(test_dovi_detect PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${_kit}/tests/unit")
    target_link_libraries(test_dovi_detect PRIVATE wnk_media)
    add_test(NAME unit.vplayer.dovi_detect COMMAND test_dovi_detect)
    add_executable(test_network tests/test_network.cpp network.cpp vlc_engine.cpp)
    target_include_directories(test_network PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${_kit}/tests/unit")
    target_include_directories(test_network SYSTEM PRIVATE "${WNK_KIT_DIR}/third_party")
    target_link_libraries(test_network PRIVATE wnk_media wnk_platform wnk_storage wnk_async)
    add_test(NAME unit.vplayer.network COMMAND test_network)
    add_executable(test_history tests/test_history.cpp history.cpp)
    target_include_directories(test_history PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${_kit}/tests/unit")
    target_include_directories(test_history SYSTEM PRIVATE "${WNK_KIT_DIR}/third_party")
    target_link_libraries(test_history PRIVATE wnk_storage wnk_async)
    add_test(NAME unit.vplayer.history COMMAND test_history)
    add_executable(test_mkv_attachments tests/test_mkv_attachments.cpp player/mkv_attachments.cpp player/ebml.cpp)
    target_include_directories(test_mkv_attachments PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${_kit}/tests/unit")
    target_link_libraries(test_mkv_attachments PRIVATE wnk_media)
    add_test(NAME unit.vplayer.mkv_attachments COMMAND test_mkv_attachments)
    add_executable(test_playlist tests/test_playlist.cpp playlist.cpp)
    target_include_directories(test_playlist PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${_kit}/tests/unit")
    target_include_directories(test_playlist SYSTEM PRIVATE "${WNK_KIT_DIR}/third_party")
    target_link_libraries(test_playlist PRIVATE wnk_media)
    add_test(NAME unit.vplayer.playlist COMMAND test_playlist)
    add_executable(test_music tests/test_music.cpp music.cpp)
    target_include_directories(test_music PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${_kit}/tests/unit")
    target_link_libraries(test_music PRIVATE wnk_platform)
    target_include_directories(test_music SYSTEM PRIVATE "${WNK_KIT_DIR}/third_party")
    add_test(NAME unit.vplayer.music COMMAND test_music)
    add_executable(test_library tests/test_library.cpp library.cpp)
    target_include_directories(test_library PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${_kit}/tests/unit")
    target_link_libraries(test_library PRIVATE wnk_platform)
    add_test(NAME unit.vplayer.library COMMAND test_library)
    # search.cpp's matcher only; the screen needs the whole app.
    add_executable(test_search tests/test_search.cpp search.cpp library.cpp network.cpp history.cpp app_settings.cpp ui/theme.cpp)
    target_include_directories(test_search PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${_kit}/tests/unit")
    target_include_directories(test_search SYSTEM PRIVATE "${WNK_KIT_DIR}/third_party")
    target_link_libraries(test_search PRIVATE wnk_app wnk_media)
    add_test(NAME unit.vplayer.search COMMAND test_search)
    add_executable(test_settings tests/test_settings.cpp app_settings.cpp)
    target_include_directories(test_settings PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${_kit}/tests/unit")
    target_include_directories(test_settings SYSTEM PRIVATE "${WNK_KIT_DIR}/third_party")
    target_link_libraries(test_settings PRIVATE wnk_app wnk_media)
    add_test(NAME unit.vplayer.settings COMMAND test_settings)
    add_executable(test_thumbnails tests/test_thumbnails.cpp thumbnails.cpp)
    target_include_directories(test_thumbnails PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${_kit}/tests/unit")
    target_link_libraries(test_thumbnails PRIVATE wnk_media wnk_platform wnk_async)
    add_test(NAME unit.vplayer.thumbnails COMMAND test_thumbnails)
    add_executable(test_teletext tests/test_teletext.cpp player/teletext.cpp)
    target_include_directories(test_teletext PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${_kit}/tests/unit")
    target_link_libraries(test_teletext PRIVATE wnk_media)
    add_test(NAME unit.vplayer.teletext COMMAND test_teletext)
    add_executable(test_cea608 tests/test_cea608.cpp player/cea608.cpp)
    target_include_directories(test_cea608 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${_kit}/tests/unit")
    target_link_libraries(test_cea608 PRIVATE wnk_media)
    add_test(NAME unit.vplayer.cea608 COMMAND test_cea608)
endif ()

# Host smoke test: the Video page scans for drives and reports, then Back twice at the root quits.
wnk_add_app_test(NAME e2e.vplayer.starts_and_quits TARGET vplayer PREFIX APP_VPLAYER
        KEYS "wait@800,Escape,Escape" PASS "home ready")
