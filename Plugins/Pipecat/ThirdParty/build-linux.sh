#!/usr/bin/env bash
#
# Copyright (c) 2026, Daily
#
# Builds the Pipecat C++ client and its WebSocket transport with Unreal Engine's
# toolchain, and installs them in ThirdParty/Linux for the Pipecat plugin, with
# nlohmann/json's headers. They use Unreal's libcurl and OpenSSL, and download
# and build what else they need.
#
# Usage:
#   UE_ROOT=/path/to/UnrealEngine \
#   PIPECAT_CLIENT_CXX=/path/to/pipecat-client-cxx \
#     ./build-linux.sh
#

set -euo pipefail

: "${UE_ROOT:?Set UE_ROOT to your Unreal Engine directory}"
: "${PIPECAT_CLIENT_CXX:?Set PIPECAT_CLIENT_CXX to the pipecat-client-cxx source}"
export UE_ROOT

HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="$HERE/build/Linux"
PREFIX="$HERE/Linux"
TOOLCHAIN="$HERE/unreal-linux.cmake"

# Unreal's libcurl and OpenSSL, which the Pipecat plugin links through Unreal.
UE_THIRD_PARTY="$UE_ROOT/Engine/Source/ThirdParty"
ARCH="x86_64-unknown-linux-gnu"
CURL_DIR="$(ls -d "$UE_THIRD_PARTY"/libcurl/*/ | sort -V | tail -1)"
OPENSSL_DIR="$(ls -d "$UE_THIRD_PARTY"/OpenSSL/*/ | sort -V | tail -1)"

rm -rf "$PREFIX"

echo "*** Building the Pipecat C++ client and its WebSocket transport ***"
cmake -S "$PIPECAT_CLIENT_CXX" -B "$BUILD/pipecat" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DPIPECAT_BUILD_WEBSOCKET=ON \
    -DCURL_INCLUDE_DIR="$CURL_DIR/include" \
    -DCURL_LIBRARY="$CURL_DIR/lib/Unix/$ARCH/Release/libcurl.a" \
    -DOPENSSL_INCLUDE_DIR="$OPENSSL_DIR/include/Unix" \
    -DOPENSSL_SSL_LIBRARY="$OPENSSL_DIR/lib/Unix/$ARCH/libssl.a" \
    -DOPENSSL_CRYPTO_LIBRARY="$OPENSSL_DIR/lib/Unix/$ARCH/libcrypto.a"
cmake --build "$BUILD/pipecat"
cmake --install "$BUILD/pipecat" --prefix "$PREFIX"

# The Pipecat headers include nlohmann/json. Unreal's sysroot doesn't have it,
# so the client's build downloads it.
if [[ ! -d "$PREFIX/include/nlohmann" ]]; then
    cp -R "$BUILD/pipecat/_deps/nlohmann_json-src/include/nlohmann" "$PREFIX/include/"
fi

echo "*** Installed in $PREFIX ***"
ls "$PREFIX/lib"
