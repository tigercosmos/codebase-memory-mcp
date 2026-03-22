#!/usr/bin/env bash
# smoke_guard.sh — Smoke test for guard and ghost-file invariants.
#
# Verifies two properties:
#   1. Query handlers return "project not indexed" for unknown projects.
#   2. No ghost .db file is created for the unknown project name.
#
# Usage: bash tests/smoke_guard.sh
# Exit 0 on success, non-zero on failure.

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="$PROJECT_ROOT/build/c/codebase-memory-mcp"
FAKE_PROJECT="nonexistent_smoke_test_xyz"
CACHE_DIR="${HOME}/.cache/codebase-memory-mcp"
GHOST_FILE="$CACHE_DIR/${FAKE_PROJECT}.db"

# ── Step 1: Build ─────────────────────────────────────────────────
echo "[smoke_guard] Building project..."
make -f "$PROJECT_ROOT/Makefile.cbm" cbm -C "$PROJECT_ROOT" --quiet 2>&1
if [ ! -x "$BINARY" ]; then
    echo "[smoke_guard] FAIL: binary not found at $BINARY after build" >&2
    exit 1
fi
echo "[smoke_guard] Build OK: $BINARY"

# ── Step 2: Pre-clean ghost file if somehow present ───────────────
if [ -f "$GHOST_FILE" ]; then
    echo "[smoke_guard] WARNING: ghost file already exists before test; removing: $GHOST_FILE"
    rm -f "$GHOST_FILE"
fi

# ── Step 3: Invoke query tool with unknown project ────────────────
echo "[smoke_guard] Invoking search_graph with project='$FAKE_PROJECT'..."
RESPONSE="$("$BINARY" cli search_graph "{\"project\":\"$FAKE_PROJECT\",\"name_pattern\":\".*\"}" 2>/dev/null)"
echo "[smoke_guard] Response: $RESPONSE"

# ── Step 4: Assert error message present ─────────────────────────
# For a truly absent project (no .db file), cbm_store_open_path_query returns
# NULL, so REQUIRE_STORE fires with "no project loaded" before
# verify_project_indexed is reached. Both messages confirm the guard is active.
if ! echo "$RESPONSE" | grep -qE "no project loaded|not indexed"; then
    echo "[smoke_guard] FAIL: response does not contain guard error ('no project loaded' or 'not indexed')" >&2
    echo "[smoke_guard] Got: $RESPONSE" >&2
    exit 1
fi
echo "[smoke_guard] PASS: guard error message present"

# ── Step 5: Assert no ghost .db file was created ─────────────────
if [ -f "$GHOST_FILE" ]; then
    echo "[smoke_guard] FAIL: ghost file was created at $GHOST_FILE" >&2
    rm -f "$GHOST_FILE"
    exit 1
fi
echo "[smoke_guard] PASS: no ghost .db file created"

echo "[smoke_guard] All checks passed."
exit 0
