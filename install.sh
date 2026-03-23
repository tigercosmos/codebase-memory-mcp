#!/usr/bin/env bash
set -euo pipefail

# install.sh — One-line installer for codebase-memory-mcp.
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/DeusData/codebase-memory-mcp/main/install.sh | bash
#   curl -fsSL ... | bash -s -- --ui          # Install the UI variant (graph visualization)
#   curl -fsSL ... | bash -s -- --dir /usr/local/bin  # Custom install directory
#
# What it does:
#   1. Detects OS (macOS/Linux/Windows) and architecture (arm64/amd64)
#   2. Downloads the latest release binary from GitHub
#   3. On macOS: removes quarantine/provenance xattrs, re-signs locally
#   4. Installs to ~/.local/bin (or custom --dir)
#   5. Runs `codebase-memory-mcp install` to configure all detected coding agents

REPO="DeusData/codebase-memory-mcp"
INSTALL_DIR="$HOME/.local/bin"
VARIANT="standard"

# ── Parse arguments ──────────────────────────────────────────
for arg in "$@"; do
    case "$arg" in
        --ui)       VARIANT="ui" ;;
        --standard) VARIANT="standard" ;;
        --dir=*)    INSTALL_DIR="${arg#--dir=}" ;;
        --dir)      shift; INSTALL_DIR="$1" ;;
        --help|-h)
            echo "Usage: install.sh [--ui] [--dir <path>]"
            echo "  --ui        Install the UI variant (with graph visualization)"
            echo "  --standard  Install the standard variant (default)"
            echo "  --dir PATH  Install directory (default: ~/.local/bin)"
            exit 0
            ;;
    esac
done

# ── Detect OS ────────────────────────────────────────────────
detect_os() {
    case "$(uname -s)" in
        Darwin)          echo "darwin" ;;
        Linux)           echo "linux" ;;
        MINGW*|MSYS*|CYGWIN*) echo "windows" ;;
        *)
            echo "error: unsupported OS: $(uname -s)" >&2
            exit 1
            ;;
    esac
}

# ── Detect architecture ──────────────────────────────────────
detect_arch() {
    case "$(uname -m)" in
        arm64|aarch64)   echo "arm64" ;;
        x86_64|amd64)
            # On macOS, check if this is Apple Silicon running under Rosetta
            if [ "$(uname -s)" = "Darwin" ] && sysctl -n machdep.cpu.brand_string 2>/dev/null | grep -qi apple; then
                echo "arm64"
            else
                echo "amd64"
            fi
            ;;
        *)
            echo "error: unsupported architecture: $(uname -m)" >&2
            exit 1
            ;;
    esac
}

OS=$(detect_os)
ARCH=$(detect_arch)

echo "codebase-memory-mcp installer"
echo "  os:      $OS"
echo "  arch:    $ARCH"
echo "  variant: $VARIANT"
echo "  target:  $INSTALL_DIR/codebase-memory-mcp"
echo ""

# ── Build download URL ───────────────────────────────────────
if [ "$OS" = "windows" ]; then
    EXT="zip"
else
    EXT="tar.gz"
fi

if [ "$VARIANT" = "ui" ]; then
    ARCHIVE="codebase-memory-mcp-ui-${OS}-${ARCH}.${EXT}"
else
    ARCHIVE="codebase-memory-mcp-${OS}-${ARCH}.${EXT}"
fi

URL="https://github.com/${REPO}/releases/latest/download/${ARCHIVE}"

# ── Download ─────────────────────────────────────────────────
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

echo "Downloading ${ARCHIVE}..."
if command -v curl &>/dev/null; then
    curl -fSL --progress-bar -o "$TMPDIR/$ARCHIVE" "$URL"
elif command -v wget &>/dev/null; then
    wget -q --show-progress -O "$TMPDIR/$ARCHIVE" "$URL"
else
    echo "error: curl or wget required" >&2
    exit 1
fi

# ── Extract ──────────────────────────────────────────────────
echo "Extracting..."
cd "$TMPDIR"
if [ "$EXT" = "zip" ]; then
    unzip -q "$ARCHIVE"
else
    tar -xzf "$ARCHIVE"
fi

BINARY="$TMPDIR/codebase-memory-mcp"
if [ "$OS" = "windows" ] && [ -f "$TMPDIR/codebase-memory-mcp.exe" ]; then
    BINARY="$TMPDIR/codebase-memory-mcp.exe"
fi

if [ ! -f "$BINARY" ]; then
    echo "error: binary not found after extraction" >&2
    exit 1
fi

# ── macOS: fix signing ───────────────────────────────────────
if [ "$OS" = "darwin" ]; then
    echo "Fixing macOS code signing..."

    # Remove quarantine and provenance xattrs (set by browsers/curl on macOS)
    xattr -d com.apple.quarantine "$BINARY" 2>/dev/null || true
    xattr -d com.apple.provenance "$BINARY" 2>/dev/null || true

    # Re-sign locally with ad-hoc signature (required for Apple Silicon)
    codesign --force --sign - "$BINARY" 2>/dev/null || true
fi

# ── Install ──────────────────────────────────────────────────
mkdir -p "$INSTALL_DIR"
DEST="$INSTALL_DIR/codebase-memory-mcp"
if [ "$OS" = "windows" ]; then
    DEST="${DEST}.exe"
fi

# Remove old binary if present (handles read-only files)
if [ -f "$DEST" ]; then
    rm -f "$DEST"
fi

cp "$BINARY" "$DEST"
chmod 755 "$DEST"

# Verify binary runs
echo ""
VERSION=$("$DEST" --version 2>&1) || {
    echo "error: installed binary failed to run" >&2
    echo "  try: xattr -cr $DEST && codesign --force --sign - $DEST" >&2
    exit 1
}
echo "Installed: $VERSION"

# ── Run install subcommand ───────────────────────────────────
echo ""
echo "Configuring coding agents..."
"$DEST" install -y 2>&1 || {
    echo ""
    echo "Agent configuration failed (non-fatal)."
    echo "You can run it manually: codebase-memory-mcp install"
}

# ── PATH check ───────────────────────────────────────────────
if ! echo "$PATH" | tr ':' '\n' | grep -qx "$INSTALL_DIR"; then
    echo ""
    echo "NOTE: $INSTALL_DIR is not in your PATH."
    echo "Add it to your shell config:"
    echo ""
    echo "  echo 'export PATH=\"$INSTALL_DIR:\$PATH\"' >> ~/.zshrc"
    echo ""
fi

echo ""
echo "Done! Restart your coding agent to start using codebase-memory-mcp."
