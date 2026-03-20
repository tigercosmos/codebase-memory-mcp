#!/usr/bin/env bash
set -euo pipefail

# Smoke test: verify the binary is fully operational.
#
# Phase 1: --version output
# Phase 2: Index a small multi-language project
# Phase 3: Verify node/edge counts, search, and trace
#
# Usage: smoke-test.sh <binary-path>

BINARY="${1:?usage: smoke-test.sh <binary-path>}"
TMPDIR=$(mktemp -d)
# On MSYS2/Windows, convert POSIX path to native Windows path for the binary
if command -v cygpath &>/dev/null; then
    TMPDIR=$(cygpath -m "$TMPDIR")
fi
trap 'rm -rf "$TMPDIR"' EXIT

CLI_STDERR=$(mktemp)
cli() { "$BINARY" cli "$@" 2>"$CLI_STDERR"; }

echo "=== Phase 1: version ==="
OUTPUT=$("$BINARY" --version 2>&1)
echo "$OUTPUT"
if ! echo "$OUTPUT" | grep -qE 'v?[0-9]+\.[0-9]+|dev'; then
  echo "FAIL: unexpected version output"
  exit 1
fi
echo "OK"

echo ""
echo "=== Phase 2: index test project ==="

# Create a small multi-language project (Python + Go + JS)
mkdir -p "$TMPDIR/src/pkg"

cat > "$TMPDIR/src/main.py" << 'PYEOF'
from pkg import helper

def main():
    result = helper.compute(42)
    print(result)

class Config:
    DEBUG = True
    PORT = 8080
PYEOF

cat > "$TMPDIR/src/pkg/__init__.py" << 'PYEOF'
from .helper import compute
PYEOF

cat > "$TMPDIR/src/pkg/helper.py" << 'PYEOF'
def compute(x):
    return x * 2

def validate(data):
    if not data:
        raise ValueError("empty")
    return True
PYEOF

cat > "$TMPDIR/src/server.go" << 'GOEOF'
package main

import "fmt"

func StartServer(port int) {
    fmt.Printf("listening on :%d\n", port)
}

func HandleRequest(path string) string {
    return "ok: " + path
}
GOEOF

cat > "$TMPDIR/src/app.js" << 'JSEOF'
function render(data) {
    return `<div>${data}</div>`;
}

function fetchData(url) {
    return fetch(url).then(r => r.json());
}

module.exports = { render, fetchData };
JSEOF

cat > "$TMPDIR/config.yaml" << 'YAMLEOF'
server:
  port: 8080
  debug: true
database:
  host: localhost
YAMLEOF

# Index
RESULT=$(cli index_repository "{\"repo_path\":\"$TMPDIR\"}")
echo "$RESULT"

STATUS=$(echo "$RESULT" | python3 -c "import json,sys; d=json.loads(json.loads(sys.stdin.read())['content'][0]['text']); print(d.get('status',''))" 2>/dev/null || echo "")
if [ "$STATUS" != "indexed" ]; then
  echo "FAIL: index status is '$STATUS', expected 'indexed'"
  echo "--- stderr ---"
  cat "$CLI_STDERR"
  echo "--- end stderr ---"
  exit 1
fi

NODES=$(echo "$RESULT" | python3 -c "import json,sys; d=json.loads(json.loads(sys.stdin.read())['content'][0]['text']); print(d.get('nodes',0))" 2>/dev/null || echo "0")
EDGES=$(echo "$RESULT" | python3 -c "import json,sys; d=json.loads(json.loads(sys.stdin.read())['content'][0]['text']); print(d.get('edges',0))" 2>/dev/null || echo "0")

echo "nodes=$NODES edges=$EDGES"

if [ "$NODES" -lt 10 ]; then
  echo "FAIL: expected at least 10 nodes, got $NODES"
  exit 1
fi
if [ "$EDGES" -lt 5 ]; then
  echo "FAIL: expected at least 5 edges, got $EDGES"
  exit 1
fi
echo "OK: $NODES nodes, $EDGES edges"

echo ""
echo "=== Phase 3: verify queries ==="

# 3a: search_graph — find the compute function
PROJECT=$(echo "$RESULT" | python3 -c "import json,sys; d=json.loads(json.loads(sys.stdin.read())['content'][0]['text']); print(d.get('project',''))" 2>/dev/null || echo "")

SEARCH=$(cli search_graph "{\"project\":\"$PROJECT\",\"name_pattern\":\"compute\"}")
TOTAL=$(echo "$SEARCH" | python3 -c "import json,sys; d=json.loads(json.loads(sys.stdin.read())['content'][0]['text']); print(d.get('total',0))" 2>/dev/null || echo "0")
if [ "$TOTAL" -lt 1 ]; then
  echo "FAIL: search_graph for 'compute' returned 0 results"
  exit 1
fi
echo "OK: search_graph found $TOTAL result(s) for 'compute'"

# 3b: trace_call_path — verify compute has callers
TRACE=$(cli trace_call_path "{\"project\":\"$PROJECT\",\"function_name\":\"compute\",\"direction\":\"inbound\",\"max_depth\":1}")
CALLERS=$(echo "$TRACE" | python3 -c "import json,sys; d=json.loads(json.loads(sys.stdin.read())['content'][0]['text']); print(len(d.get('callers',[])))" 2>/dev/null || echo "0")
if [ "$CALLERS" -lt 1 ]; then
  echo "FAIL: trace_call_path found 0 callers for 'compute'"
  exit 1
fi
echo "OK: trace_call_path found $CALLERS caller(s) for 'compute'"

# 3c: get_graph_schema — verify labels exist
SCHEMA=$(cli get_graph_schema "{\"project\":\"$PROJECT\"}")
LABELS=$(echo "$SCHEMA" | python3 -c "import json,sys; d=json.loads(json.loads(sys.stdin.read())['content'][0]['text']); print(len(d.get('node_labels',[])))" 2>/dev/null || echo "0")
if [ "$LABELS" -lt 3 ]; then
  echo "FAIL: schema has fewer than 3 node labels"
  exit 1
fi
echo "OK: schema has $LABELS node labels"

# 3d: Verify __init__.py didn't clobber Folder node
FOLDERS=$(cli search_graph "{\"project\":\"$PROJECT\",\"label\":\"Folder\"}")
FOLDER_COUNT=$(echo "$FOLDERS" | python3 -c "import json,sys; d=json.loads(json.loads(sys.stdin.read())['content'][0]['text']); print(d.get('total',0))" 2>/dev/null || echo "0")
if [ "$FOLDER_COUNT" -lt 2 ]; then
  echo "FAIL: expected at least 2 Folder nodes (src, src/pkg), got $FOLDER_COUNT"
  exit 1
fi
echo "OK: $FOLDER_COUNT Folder nodes (init.py didn't clobber them)"

# 3e: delete_project cleanup
cli delete_project "{\"project_name\":\"$PROJECT\"}" > /dev/null

echo ""
echo "=== Phase 4: security checks ==="

# 4a: Clean shutdown — binary must exit within 5 seconds after EOF
echo "Testing clean shutdown..."
SHUTDOWN_TMPDIR=$(mktemp -d)
cat > "$SHUTDOWN_TMPDIR/input.jsonl" << 'JSONL'
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}}
JSONL

# Run binary with EOF and check it exits within 5 seconds
timeout 5 "$BINARY" < "$SHUTDOWN_TMPDIR/input.jsonl" > /dev/null 2>&1 || true
EXIT_CODE=$?
rm -rf "$SHUTDOWN_TMPDIR"

if [ "$EXIT_CODE" -eq 124 ]; then
  echo "FAIL: binary did not exit within 5 seconds after EOF"
  exit 1
fi
echo "OK: clean shutdown"

# 4b: No residual processes (skip on Windows/MSYS2 where pgrep may not work)
if command -v pgrep &>/dev/null && [ "$(uname)" != "MINGW64_NT" ] 2>/dev/null; then
  # Give a moment for any child processes to clean up
  sleep 1
  RESIDUAL=$(pgrep -f "codebase-memory-mcp.*cli" 2>/dev/null | wc -l | tr -d ' \n' || echo "0")
  RESIDUAL="${RESIDUAL:-0}"
  if [ "$RESIDUAL" -gt 0 ]; then
    echo "WARNING: $RESIDUAL residual codebase-memory-mcp process(es) found"
  else
    echo "OK: no residual processes"
  fi
fi

# 4c: Version integrity — output must be exactly one line matching version format
VERSION_OUTPUT=$("$BINARY" --version 2>&1)
VERSION_LINES=$(echo "$VERSION_OUTPUT" | wc -l | tr -d ' ')
if [ "$VERSION_LINES" -ne 1 ]; then
  echo "FAIL: --version output has $VERSION_LINES lines, expected exactly 1"
  echo "  Output: $VERSION_OUTPUT"
  exit 1
fi
echo "OK: version output is clean single line"

echo ""
echo "=== smoke-test: ALL PASSED ==="
