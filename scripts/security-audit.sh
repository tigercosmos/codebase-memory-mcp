#!/usr/bin/env bash
set -euo pipefail

# Layer 1: Static security audit — scans C source for dangerous calls.
# Every occurrence must be on the checked-in allow-list.
#
# Usage: scripts/security-audit.sh

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ALLOWLIST="$ROOT/scripts/security-allowlist.txt"

if [[ ! -f "$ALLOWLIST" ]]; then
    echo "FAIL: allow-list not found: $ALLOWLIST"
    exit 1
fi

# Use a flag file to communicate failures from subshells
FAIL_FLAG=$(mktemp)
echo "0" > "$FAIL_FLAG"
trap 'rm -f "$FAIL_FLAG"' EXIT

fail() {
    echo "1" > "$FAIL_FLAG"
}

# ── 1. Dangerous function calls ─────────────────────────────────

echo "=== Layer 1: Static Security Audit ==="
echo ""
echo "--- Scanning for dangerous function calls ---"

# For each file:function pair on the allow-list, the file is allowed to contain
# that function. Any file:function NOT on the list causes failure.
FUNC_LIST="system popen cbm_popen execl fork"

while IFS= read -r file; do
    relfile="${file#"$ROOT/"}"

    for func in $FUNC_LIST; do
        # Build precise grep pattern to avoid substring matches:
        # 'popen(' must NOT match 'cbm_popen(' — use [^a-z] negative class
        case "$func" in
            cbm_popen) pattern="cbm_popen(" ;;
            popen)     pattern="[^a-z]popen(" ;;
            system)    pattern="[^a-z_]system(" ;;
            fork)      pattern="[^a-z_]fork(" ;;
            *)         pattern="[^a-z_]${func}(" ;;
        esac

        # Grep for pattern, excluding comments and #define lines
        if grep -n "$pattern" "$file" 2>/dev/null | grep -v '^\s*//' | grep -v '^\s*\*' | grep -v '#define' > /dev/null 2>&1; then
            if ! grep -q "^${relfile}:${func}:" "$ALLOWLIST" 2>/dev/null; then
                echo "BLOCKED: ${relfile}: contains ${func}() — not on allow-list"
                grep -n "$pattern" "$file" 2>/dev/null | grep -v '^\s*//' | grep -v '^\s*\*' | grep -v '#define' | head -3 | sed 's/^/  /'
                fail
            fi
        fi
    done
done < <(find "$ROOT/src" -name '*.c' -type f | sort)

# ── 1b. Raw network calls (must not exist) ──────────────────────

echo ""
echo "--- Scanning for raw network calls (must not exist) ---"

NETWORK_FUNCS='[^a-z_]connect\(|[^a-z_]socket\(|[^a-z_]sendto\('
if grep -rn -E "$NETWORK_FUNCS" "$ROOT/src/" --include='*.c' 2>/dev/null | grep -v '^\s*//' | grep -v '^\s*\*' | grep -v 'test'; then
    echo "BLOCKED: Raw network calls found in src/."
    fail
else
    echo "OK: No raw network calls found."
fi

# ── 2. Hardcoded URLs in string literals ─────────────────────────

echo ""
echo "--- Scanning for hardcoded URLs ---"

# Extract allowed URL prefixes from the allow-list
# Format: URL:<url>:<justification>
ALLOWED_URLS=()
while IFS= read -r line; do
    [[ "$line" =~ ^#.*$ ]] && continue
    [[ -z "$line" ]] && continue
    if [[ "$line" =~ ^URL: ]]; then
        rest="${line#URL:}"
        # Extract URL (scheme://host/path) — stop at first colon that follows a non-slash
        if [[ "$rest" =~ ^(https?://[^[:space:]]+) ]]; then
            # The URL part extends until the justification separator
            # Use the fact that justifications follow the pattern ":word word"
            url_part="${BASH_REMATCH[1]}"
            # Remove trailing justification after last colon that precedes a space
            url_part="${url_part%%:[A-Za-z]*}"
            ALLOWED_URLS+=("$url_part")
        fi
    fi
done < "$ALLOWLIST"

URL_OK=true

# Non-functional URL patterns to skip (comments, placeholders, comparisons, patterns)
is_placeholder_url() {
    local url="$1"
    case "$url" in
        # Placeholder/example URLs in comments and code
        https://host/*|http://host/*) return 0 ;;
        https://...*|http://...*) return 0 ;;
        # Protocol prefix comparisons (strncmp, mg_match)
        http://) return 0 ;;
        https://) return 0 ;;
        # mg_match glob patterns with wildcards
        http://localhost:*) return 0 ;;
        http://127.0.0.1:*) return 0 ;;
    esac
    return 1
}

while IFS= read -r file; do
    relfile="${file#"$ROOT/"}"

    # Find lines with URLs (excluding comments)
    while IFS= read -r match; do
        [[ -z "$match" ]] && continue

        # Skip lines that are clearly comments (/* ... */ or // ...)
        line_content="${match#*:}"  # Remove line number prefix
        # Skip if the URL appears only in a comment on this line
        if echo "$line_content" | grep -qE '^\s*/[/*]'; then
            continue
        fi

        # Extract URLs using grep -oE (POSIX-compatible)
        while IFS= read -r url; do
            [[ -z "$url" ]] && continue

            # Skip non-functional placeholder URLs
            if is_placeholder_url "$url"; then
                continue
            fi

            allowed=false
            for aurl in "${ALLOWED_URLS[@]+"${ALLOWED_URLS[@]}"}"; do
                if [[ "$url" == "$aurl"* ]]; then
                    allowed=true
                    break
                fi
            done

            if ! $allowed; then
                echo "BLOCKED: ${relfile}: URL not on allow-list: $url"
                fail
                URL_OK=false
            fi
        done < <(echo "$match" | grep -oE 'https?://[A-Za-z0-9._/~:@!$&()*+,;=?#%-]+' || true)
    done < <(grep -n 'https\?://' "$file" 2>/dev/null | grep -v '^\s*//' | grep -v '^\s*\*' || true)
done < <(find "$ROOT/src" -name '*.c' -type f | sort)

if $URL_OK; then
    echo "OK: All URLs are on the allow-list."
fi

# ── 3. File writes outside expected paths ────────────────────────

echo ""
echo "--- Scanning for unexpected file writes in src/ ---"

FOPEN_FOUND=false
while IFS= read -r match; do
    [[ -z "$match" ]] && continue
    file=$(echo "$match" | cut -d: -f1)
    relfile="${file#"$ROOT/"}"
    case "$relfile" in
        src/cli/cli.c|src/store/store.c|src/pipeline/*.c|src/foundation/log.c|src/ui/http_server.c|src/ui/config.c|src/mcp/mcp.c)
            ;; # Known safe
        *)
            echo "REVIEW: ${match}"
            echo "  -> Unexpected fopen(\"w\") in ${relfile}"
            FOPEN_FOUND=true
            ;;
    esac
done < <(grep -rn 'fopen.*"w' "$ROOT/src/" --include='*.c' 2>/dev/null | grep -v '/test' | grep -v '^\s*//' || true)

if ! $FOPEN_FOUND; then
    echo "OK: All file writes are in expected locations."
fi

# ── Summary ──────────────────────────────────────────────────────

echo ""
RESULT=$(cat "$FAIL_FLAG")
if [[ "$RESULT" != "0" ]]; then
    echo "=== SECURITY AUDIT FAILED ==="
    echo "Fix the issues above or add entries to scripts/security-allowlist.txt with justifications."
    exit 1
fi

echo "=== Security audit passed ==="
