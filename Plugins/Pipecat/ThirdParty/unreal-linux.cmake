#
# Copyright (c) 2026, Daily
#
# CMake toolchain that builds with Unreal Engine's Linux toolchain: its clang,
# sysroot and libc++. Libraries built with it can be linked into Unreal
# modules. Set UE_ROOT to the engine.
#

if(NOT DEFINED ENV{UE_ROOT})
  message(FATAL_ERROR "Set UE_ROOT to your Unreal Engine directory")
endif()

file(GLOB _ue_toolchains
  "$ENV{UE_ROOT}/Engine/Extras/ThirdPartyNotUE/SDKs/HostLinux/Linux_x64/*/x86_64-unknown-linux-gnu"
)
list(GET _ue_toolchains 0 UE_TOOLCHAIN)

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_SYSROOT "${UE_TOOLCHAIN}")

set(CMAKE_C_COMPILER "${UE_TOOLCHAIN}/bin/clang")
set(CMAKE_CXX_COMPILER "${UE_TOOLCHAIN}/bin/clang++")

# Unreal modules are shared libraries, and use libc++.
set(CMAKE_C_FLAGS_INIT "-fPIC")
set(CMAKE_CXX_FLAGS_INIT "-fPIC -stdlib=libc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld -stdlib=libc++ -lc++abi")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld -stdlib=libc++ -lc++abi")

# Programs come from the host. Libraries only come from Unreal's sysroot, or
# are given to the build, since the host's aren't built for Unreal.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
