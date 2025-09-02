#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DEBUG="$ROOT/build-debug"
BUILD_RELEASE="$ROOT/build-release"

usage() {
    cat <<EOF
Usage: $(basename "$0") [debug|release|all|clean]

Commands:
  debug    Configure and build Debug in ${BUILD_DEBUG}
  release  Configure and build Release in ${BUILD_RELEASE}
  all      Build both debug and release (default)
  clean    Remove build-debug and build-release directories
EOF
}

cmd=${1:-all}

case "$cmd" in
    debug)
        mkdir -p "$BUILD_DEBUG"
        cmake -S "$ROOT" -B "$BUILD_DEBUG" -DCMAKE_BUILD_TYPE=Debug
        cmake --build "$BUILD_DEBUG" -j
        ;;
    release)
        mkdir -p "$BUILD_RELEASE"
        cmake -S "$ROOT" -B "$BUILD_RELEASE" -DCMAKE_BUILD_TYPE=Release
        cmake --build "$BUILD_RELEASE" -j
        ;;
    all)
        mkdir -p "$BUILD_DEBUG" "$BUILD_RELEASE"
        cmake -S "$ROOT" -B "$BUILD_DEBUG" -DCMAKE_BUILD_TYPE=Debug
        cmake --build "$BUILD_DEBUG" -j
        cmake -S "$ROOT" -B "$BUILD_RELEASE" -DCMAKE_BUILD_TYPE=Release
        cmake --build "$BUILD_RELEASE" -j
        ;;
    clean)
        rm -rf "$BUILD_DEBUG" "$BUILD_RELEASE"
        echo "Removed $BUILD_DEBUG and $BUILD_RELEASE"
        ;;
    -h|--help)
        usage
        ;;
    *)
        echo "Unknown command: $cmd"
        usage
        exit 1
        ;;
esac
