#!/bin/bash
# build.sh — Clean build of production binary (standard or with UI).
#
# Usage:
#   scripts/build.sh                              # Standard binary
#   scripts/build.sh --with-ui                    # Binary with embedded UI
#   scripts/build.sh --version v0.8.0             # With version stamp
#   scripts/build.sh --arch x86_64                # Force x86_64 build
#   scripts/build.sh CC=gcc-14 CXX=g++-14        # Override compiler
#
# This script is the SINGLE source of truth for building release binaries.
# Used identically in local development and CI workflows.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# Pre-parse --arch flag before sourcing env.sh
for arg in "$@"; do
    case "$arg" in
        --arch=*) export CBM_ARCH="${arg#--arch=}" ;;
    esac
done
prev_arg=""
for arg in "$@"; do
    if [[ "${prev_arg:-}" == "--arch" ]]; then
        export CBM_ARCH="$arg"
    fi
    prev_arg="$arg"
done

# shellcheck source=env.sh
source "$ROOT/scripts/env.sh"

# Parse remaining arguments
WITH_UI=false
VERSION=""
EXTRA_MAKE_ARGS=()

prev_arg=""
for arg in "$@"; do
    # Skip --arch and its value (already handled)
    if [[ "${prev_arg:-}" == "--arch" ]]; then
        prev_arg="$arg"
        continue
    fi
    case "$arg" in
        --with-ui)
            WITH_UI=true
            ;;
        --version)
            prev_arg="$arg"
            continue
            ;;
        --arch|--arch=*)
            ;; # already handled
        CC=*|CXX=*)
            export "${arg}"
            EXTRA_MAKE_ARGS+=("$arg")
            ;;
        *)
            # Check if this is the value for --version
            if [[ "${prev_arg:-}" == "--version" ]]; then
                VERSION="$arg"
            else
                EXTRA_MAKE_ARGS+=("$arg")
            fi
            ;;
    esac
    prev_arg="$arg"
done

print_env "build.sh"
echo "  ui=$WITH_UI version=${VERSION:-dev}"

# Verify compiler supports target arch
verify_compiler "$CC"

# Map the build knobs to CMake options. CC/CXX are already exported above;
# pass them explicitly so a re-run reconfigures cleanly. STATIC=1 (passed
# through EXTRA_MAKE_ARGS) -> -DCBM_STATIC=ON.
# -O2 WITHOUT NDEBUG (mirrors Makefile CFLAGS_PROD): the Release/RelWithDebInfo
# build types define NDEBUG, which a vendored tree-sitter scanner (just) rejects
# with #error, and assertions are kept on in prod. CBM_WERROR is off for the
# prod build because the legacy C++ bodies are not yet -Werror-clean at -O2
# (warnings are gated in the lint stage, not the build).
CMAKE_ARGS=(-DCBM_SANITIZE=OFF -DCBM_WERROR=OFF -DCMAKE_BUILD_TYPE=None
            -DCMAKE_C_FLAGS=-O2 -DCMAKE_CXX_FLAGS=-O2)
[[ -n "${CC:-}" ]]  && CMAKE_ARGS+=(-DCMAKE_C_COMPILER="$CC")
[[ -n "${CXX:-}" ]] && CMAKE_ARGS+=(-DCMAKE_CXX_COMPILER="$CXX")
[[ -n "$VERSION" ]] && CMAKE_ARGS+=(-DCBM_VERSION="${VERSION#v}")
for a in "${EXTRA_MAKE_ARGS[@]+"${EXTRA_MAKE_ARGS[@]}"}"; do
    case "$a" in
        STATIC=1) CMAKE_ARGS+=(-DCBM_STATIC=ON) ;;
    esac
done

# Step 1: Clean C build artifacts only (not node_modules — npm ci handles that)
rm -rf "$ROOT/build/c"

# Step 2 (UI only): build the frontend and embed assets, same prerequisite as
# the old `cbm-with-ui: embed` rule. Generates src/ui/embedded_assets.c plus
# embed_*.o linkables in build/embedded, which CMake picks up under CBM_WITH_UI.
if $WITH_UI; then
    (cd "$ROOT/graph-ui" && npm ci && npm run build)
    "$ROOT/scripts/embed-frontend.sh" "$ROOT/graph-ui/dist" "$ROOT/build/embedded"
    CMAKE_ARGS+=(-DCBM_WITH_UI=ON)
fi

# Step 3: Configure + build the production binary at build/c/codebase-memory-mcp
$ARCH_PREFIX cmake -S "$ROOT" -B "$ROOT/build/c" "${CMAKE_ARGS[@]}"
$ARCH_PREFIX cmake --build "$ROOT/build/c" -j"$NPROC" --target codebase-memory-mcp

echo "=== Build complete: build/c/codebase-memory-mcp ==="
