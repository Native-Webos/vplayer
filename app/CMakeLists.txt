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

add_executable(vplayer main.cpp)
target_link_libraries(vplayer PRIVATE wnk_app)
target_include_directories(vplayer SYSTEM PRIVATE "${WNK_KIT_DIR}/third_party")  # nlohmann/json.hpp

# Fonts (Atkinson Hyperlegible, OFL) live in fonts/ next to the executable, on the host and in the ipk.
add_custom_command(TARGET vplayer POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${WNK_FONT_DIR}" "$<TARGET_FILE_DIR:vplayer>/fonts"
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
        EXTRA_DIRS "${WNK_FONT_DIR}")

# Host smoke test: OK on the focused button, then Back twice at the root quits.
wnk_add_app_test(NAME e2e.vplayer.starts_and_quits TARGET vplayer PREFIX APP_VPLAYER
        KEYS "Return,Escape,Escape" PASS "hello pressed 1")
