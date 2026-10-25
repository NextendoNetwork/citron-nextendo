# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(tests
    common/bit_field.cpp
    common/cityhash.cpp
    common/container_hash.cpp
    common/fibers.cpp
    common/host_memory.cpp
    common/logging.cpp
    common/nextendo_compatible_titles.cpp
    common/param_package.cpp
    common/range_map.cpp
    common/ring_buffer.cpp
    common/scratch_buffer.cpp
    common/unique_function.cpp
    common/uint128.cpp
    core/core_timing.cpp
    core/loader/nextendo_amongus_patch.cpp
    core/loader/nextendo_jd22_patch.cpp
    core/hle/service/acc/nextendo_account_country.cpp
    core/hle/service/olsc/nextendo_island.cpp
    core/hle/service/sockets/nextendo_uch.cpp
    core/internal_network/network.cpp
    precompiled_headers.h
    video_core/memory_tracker.cpp
    video_core/vp9_segmentation.cpp
    input_common/calibration_configuration_job.cpp
)

# Test translation units are not unity-safe.
set_property(TARGET tests PROPERTY UNITY_BUILD OFF)

create_target_directory_groups(tests)

target_link_libraries(tests PRIVATE common core input_common)
target_link_libraries(tests PRIVATE ${PLATFORM_LIBRARIES} Catch2::Catch2WithMain Threads::Threads)

add_test(NAME tests COMMAND tests)

if (CITRON_USE_PRECOMPILED_HEADERS)
    target_precompile_headers(tests PRIVATE precompiled_headers.h)
endif()
