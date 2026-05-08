// lsp_all.c — Compilation unit for all LSP type resolver source files.
// CGo only compiles .c files in the package directory, not subdirectories.
// This file includes all LSP sources so they compile as part of the cbm package.

#include "lsp/type_rep.c"
#include "lsp/scope.c"
#include "lsp/type_registry.c"
#include "lsp/go_lsp.c"
#include "lsp/generated/go_stdlib_data.c"
#include "lsp/c_lsp.c"
#include "lsp/generated/c_stdlib_data.c"
#include "lsp/generated/cpp_stdlib_data.c"
#include "lsp/php_lsp.c"
#include "lsp/generated/php_stdlib_data.c"
