/*
 * pass_lsp_cross.h — Cross-file LSP type-aware call resolution pass.
 *
 * Per-file LSP (cbm_run_X_lsp inside cbm_extract_file) only sees a single
 * file's defs in its registry, so callees whose receiver type comes from
 * an imported module stay unresolved. This pass closes that gap: for each
 * indexed file, it builds a project-wide CBMLSPDef[] (covering ALL files'
 * defs) plus the file's resolved import map (from gbuf IMPORTS edges),
 * then calls the language-specific cbm_run_X_lsp_cross. The resulting
 * CBMResolvedCall entries are appended to each file's resolved_calls so
 * the call-resolution pass picks them up via the same shared helper
 * (cbm_pipeline_find_lsp_resolution) that already handles per-file LSP.
 *
 * Languages covered: Go, C/C++/CUDA, Python, TypeScript/JavaScript/JSX/TSX,
 * PHP. PHP cross-file landed alongside this pass — see cbm_run_php_lsp_cross.
 *
 * Public declaration of cbm_pipeline_pass_lsp_cross lives in
 * pipeline_internal.h alongside the other pipeline passes; this header is
 * a stub kept for source-file readability (mirrors the dedicated header
 * pattern of pass_cross_repo.h, but no public API surface here).
 */
#ifndef CBM_PIPELINE_PASS_LSP_CROSS_H
#define CBM_PIPELINE_PASS_LSP_CROSS_H

#include "pipeline/pipeline_internal.h"

#endif /* CBM_PIPELINE_PASS_LSP_CROSS_H */
