# SPDX-FileCopyrightText: 2023 yuzu Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

add_library(citron-android SHARED
    emu_window/emu_window.cpp
    emu_window/emu_window.h
    native.cpp
    native.h
    nextendo_jni.cpp
    native_config.cpp
    android_settings.cpp
    game_metadata.cpp
    native_log.cpp
    android_config.cpp
    android_config.h
    native_input.cpp
    ${CMAKE_SOURCE_DIR}/src/citron/nextendo_byaml.cpp
    ${CMAKE_SOURCE_DIR}/src/citron/nextendo_byaml.h
    ${CMAKE_SOURCE_DIR}/src/citron/nextendo_mii.cpp
    ${CMAKE_SOURCE_DIR}/src/citron/nextendo_mii.h
    ${CMAKE_SOURCE_DIR}/src/citron/nextendo_save_sync.cpp
    ${CMAKE_SOURCE_DIR}/src/citron/nextendo_save_sync.h
    ${CMAKE_SOURCE_DIR}/src/citron/nextendo_ssbu_mods.cpp
    ${CMAKE_SOURCE_DIR}/src/citron/nextendo_ssbu_mods.h
)

set_property(TARGET citron-android PROPERTY IMPORTED_LOCATION ${FFmpeg_LIBRARY_DIR})

target_include_directories(citron-android PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_compile_definitions(citron-android PRIVATE CITRON_ENABLE_LIBARCHIVE)
target_compile_definitions(citron-android PRIVATE ENABLE_WEB_SERVICE)

find_package(LibArchive REQUIRED)
target_link_libraries(citron-android PRIVATE audio_core common core input_common frontend_common network web_service LibArchive::LibArchive Vulkan::Headers)
target_link_libraries(citron-android PRIVATE android camera2ndk EGL glad jnigraphics log)
if (ARCHITECTURE_arm64)
    target_link_libraries(citron-android PRIVATE adrenotools)
endif()

set(CPACK_PACKAGE_EXECUTABLES ${CPACK_PACKAGE_EXECUTABLES} citron-android)
