# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

add_library(web_service STATIC
    announce_room_json.cpp
    announce_room_json.h
    mk8d_country_flags.cpp
    mk8d_country_flags.h
    nextendo_api.cpp
    nextendo_api.h
    npln_patch_catalog.cpp
    npln_patch_catalog.h
    npln_patch_manifest.cpp
    npln_patch_manifest.h
    npln_patch_state.h
    population_scraper_client.cpp
    population_scraper_client.h
    precompiled_headers.h
    ssbu_mod_installer.cpp
    ssbu_mod_installer.h
    verify_login.cpp
    verify_login.h
    verify_user_jwt.cpp
    verify_user_jwt.h
    web_backend.cpp
    web_backend.h
    web_result.h
)

# Public desktop application metadata, never a user credential.
set(CITRON_NEXTENDO_DESKTOP_CLIENT_ID "$ENV{CITRON_NEXTENDO_DESKTOP_CLIENT_ID}"
    CACHE STRING "Public Nextendo desktop OAuth application identifier")
if (NOT CITRON_NEXTENDO_DESKTOP_CLIENT_ID STREQUAL "")
    string(LENGTH "${CITRON_NEXTENDO_DESKTOP_CLIENT_ID}" desktop_client_id_length)
    if (desktop_client_id_length GREATER 128 OR
        NOT CITRON_NEXTENDO_DESKTOP_CLIENT_ID MATCHES "^[A-Za-z0-9_-]+$")
        message(FATAL_ERROR "Invalid public Nextendo desktop OAuth application identifier")
    endif()
    target_compile_definitions(web_service PRIVATE
        CITRON_NEXTENDO_DESKTOP_CLIENT_ID="${CITRON_NEXTENDO_DESKTOP_CLIENT_ID}")
endif()

create_target_directory_groups(web_service)
target_link_libraries(web_service PRIVATE common network nlohmann_json::nlohmann_json httplib::httplib OpenSSL::Crypto)

if (CITRON_USE_PRECOMPILED_HEADERS)
    target_precompile_headers(web_service PRIVATE precompiled_headers.h)
endif()

citron_configure_lto(web_service)
