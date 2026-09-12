# SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
#
# macOS host triplet override: AGP puts the NDK clang in CC/CXX, which vcpkg's
# host-triplet detection then uses and cannot link a macOS host binary. Pin the
# host compiler to the system clang via a chainload toolchain.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/host-toolchain.cmake")