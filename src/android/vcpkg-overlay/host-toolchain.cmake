# SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Host toolchain for the macOS host triplet: real system clang, not the NDK cross compiler.
set(CMAKE_C_COMPILER /usr/bin/clang CACHE FILEPATH "host C compiler" FORCE)
set(CMAKE_CXX_COMPILER /usr/bin/clang++ CACHE FILEPATH "host C++ compiler" FORCE)