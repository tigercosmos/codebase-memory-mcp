// lsp_all.c — Compilation unit for all LSP type resolver source files.
// CGo only compiles .c files in the package directory, not subdirectories.
// This file includes all LSP sources so they compile as part of the cbm package.

#include "lsp/type_rep.cpp"
#include "lsp/scope.cpp"
#include "lsp/type_registry.cpp"
#include "lsp/go_lsp.cpp"
#include "lsp/c_lsp.cpp"
#include "lsp/php_lsp.cpp"
#include "lsp/py_lsp.cpp"
