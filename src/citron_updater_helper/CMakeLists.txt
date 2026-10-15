# SPDX-FileCopyrightText: Copyright 2025 citron Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

# Skip rather than fail the whole configure if libarchive isn't available -- citron.exe already
# degrades cleanly when this exe is missing.
if (NOT TARGET LibArchive::LibArchive)
    message(WARNING "citron-updater-helper: LibArchive::LibArchive not available, skipping.")
    return()
endif()

add_executable(citron-updater-helper main.cpp)

target_link_libraries(citron-updater-helper PRIVATE LibArchive::LibArchive)
target_compile_definitions(citron-updater-helper PRIVATE UNICODE _UNICODE)
if(MINGW)
    target_link_options(citron-updater-helper PRIVATE -municode)
endif()
set_target_properties(citron-updater-helper PROPERTIES WIN32_EXECUTABLE OFF)
