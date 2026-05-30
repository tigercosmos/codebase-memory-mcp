# Architecture Decision Record — codebase-memory-mcp

## Overview
A pure **C11** MCP (Model Context Protocol) server that indexes codebases into a
persistent knowledge graph for AI coding agents. Ships as a single static binary
(macOS/Linux/Windows, arm64+amd64) with **zero runtime dependencies**. Parses 155
languages via vendored tree-sitter grammars, augmented with LSP-style hybrid type
resolution for Go, C, C++, and TS/JS/JSX/TSX. Version 0.6.0.

## Key design decisions
- **Pure C11, zero deps, single static binary.** No Docker, no runtime, no API keys.
  Distribution handled per-platform under `pkg/` (aur, chocolatey, homebrew, npm,
  pypi, scoop, winget, go). Built via `Makefile.cbm`; Nix flake for reproducible builds.
- **RAM-first indexing pipeline.** In-memory SQLite, LZ4 compression, fused
  Aho-Corasick pattern matching. Memory released after indexing. Target: full-index
  the Linux kernel (28M LOC / 75K files) in ~3 min; structural queries in <1ms.
- **Everything compiled in.** tree-sitter grammars and C libraries are vendored
  (`vendored/`: mimalloc, mongoose HTTP, sqlite3, tre regex, xxhash, yyjson JSON) so
  nothing external can break at runtime.
- **All processing is local.** Code never leaves the machine; no telemetry.

## Module map (`src/`)
- **mcp/** — MCP protocol server: JSON-RPC + MCP tool dispatch (14 tools: search_graph,
  trace_path, get_code_snippet, query_graph, get_architecture, search_code,
  index_repository, manage_adr, etc.).
- **pipeline/** — Core indexing engine (~28 passes): `pass_definitions`, `pass_calls`,
  `pass_enrichment`, `pass_cross_repo` (CROSS_HTTP/ASYNC/CHANNEL links),
  `pass_configures`/`pass_configlink`, `pass_compile_commands`, `pass_infrascan`
  (Docker/K8s/Kustomize as graph nodes). `fqn.c` builds qualified names;
  `lsp_resolve` does hybrid type resolution; `artifact.c` writes shareable graph dumps.
- **cypher/** — Cypher query engine over the knowledge graph.
- **store/** — Persistence layer (SQLite-backed graph store).
- **graph_buffer/** — In-memory graph staging buffer during indexing.
- **semantic/** — AST profiling + semantic embedding vectors (powers semantic_query
  and SEMANTICALLY_RELATED / SIMILAR_TO edges).
- **simhash/** — MinHash/SimHash for near-duplicate / SIMILAR_TO detection.
- **discover/** — Repo discovery: gitignore parsing, language detection, user config.
- **watcher/** — File watcher for incremental re-indexing (detect_changes).
- **traces/** — Runtime trace ingestion (ingest_traces) for data-flow enrichment.
- **cli/** — CLI entry point; `hook_augment.c` powers the agent PreToolUse
  code-discovery hook; progress reporting.
- **ui/** — Optional embedded HTTP server (mongoose) + 3D force layout serving the
  graph visualization at localhost:9749.
- **foundation/** — Cross-platform substrate: arena allocator, fs/regex/thread compat
  shims, diagnostics, shared constants.

## Graph model
Nodes: Project, Folder, File, Module, Class, Interface, Enum, Type, Function, Method,
Field, Variable, Section, Route (75). Primary edges: DEFINES, USAGE, CALLS, IMPORTS,
WRITES, HTTP_CALLS, CONFIGURES, INHERITS, SIMILAR_TO, SEMANTICALLY_RELATED,
FILE_CHANGES_WITH (git co-change), RAISES/THROWS, plus CROSS_* cross-service edges.

## Other components
- **graph-ui/** — React/TSX 3D knowledge-graph visualizer (the optional `-ui` binary
  variant embeds its built assets via `src/ui/embedded_assets.h`).
- **tools/** — Custom tree-sitter grammars (`tree-sitter-form`, `tree-sitter-magma`).
- **tests/** — Large C test suite (2812 tests; `test_mcp.c`, `test_incremental.c`, etc.).
- **scripts/hooks/** — pre-commit and agent hook integration.

## Notes for future work
- The MCP `query_graph` tool currently returns a fixed column projection for some
  ad-hoc Cypher (observed: `WITH ... RETURN ext, n` came back projected as
  name/qualified_name/label). Prefer `get_architecture` / `search_graph` for
  aggregate structure questions.
- A code-discovery PreToolUse hook gates Read/Grep on code paths, steering agents to
  graph tools first; it also (recently) blocks Read on docs like README — fall back to
  Bash text reads when that happens.
