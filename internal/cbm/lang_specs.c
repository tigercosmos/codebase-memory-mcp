#include "lang_specs.h"
#include "cbm.h"             // CBMLanguage, CBM_LANG_*
#include "tree_sitter/api.h" // TSLanguage

// -- Extern declarations for tree-sitter grammar functions --
// These symbols are defined in the grammar C code compiled by Go tree-sitter modules.
extern const TSLanguage *tree_sitter_go(void);
extern const TSLanguage *tree_sitter_python(void);
extern const TSLanguage *tree_sitter_javascript(void);
extern const TSLanguage *tree_sitter_typescript(void);
extern const TSLanguage *tree_sitter_tsx(void);
extern const TSLanguage *tree_sitter_rust(void);
extern const TSLanguage *tree_sitter_java(void);
extern const TSLanguage *tree_sitter_cpp(void);
extern const TSLanguage *tree_sitter_c_sharp(void);
extern const TSLanguage *tree_sitter_php_only(void);
extern const TSLanguage *tree_sitter_lua(void);
extern const TSLanguage *tree_sitter_scala(void);
extern const TSLanguage *tree_sitter_kotlin(void);
extern const TSLanguage *tree_sitter_ruby(void);
extern const TSLanguage *tree_sitter_c(void);
extern const TSLanguage *tree_sitter_bash(void);
extern const TSLanguage *tree_sitter_zig(void);
extern const TSLanguage *tree_sitter_elixir(void);
extern const TSLanguage *tree_sitter_haskell(void);
extern const TSLanguage *tree_sitter_ocaml(void);
extern const TSLanguage *tree_sitter_objc(void);
extern const TSLanguage *tree_sitter_swift(void);
extern const TSLanguage *tree_sitter_dart(void);
extern const TSLanguage *tree_sitter_perl(void);
extern const TSLanguage *tree_sitter_groovy(void);
extern const TSLanguage *tree_sitter_erlang(void);
extern const TSLanguage *tree_sitter_r(void);
extern const TSLanguage *tree_sitter_html(void);
extern const TSLanguage *tree_sitter_css(void);
extern const TSLanguage *tree_sitter_scss(void);
extern const TSLanguage *tree_sitter_yaml(void);
extern const TSLanguage *tree_sitter_toml(void);
extern const TSLanguage *tree_sitter_hcl(void);
extern const TSLanguage *tree_sitter_sql(void);
extern const TSLanguage *tree_sitter_dockerfile(void);
// New languages (v0.5 expansion)
extern const TSLanguage *tree_sitter_clojure(void);
extern const TSLanguage *tree_sitter_fsharp(void);
extern const TSLanguage *tree_sitter_julia(void);
extern const TSLanguage *tree_sitter_vim(void);
extern const TSLanguage *tree_sitter_nix(void);
extern const TSLanguage *tree_sitter_commonlisp(void);
extern const TSLanguage *tree_sitter_elm(void);
extern const TSLanguage *tree_sitter_fortran(void);
extern const TSLanguage *tree_sitter_cuda(void);
extern const TSLanguage *tree_sitter_COBOL(void);
extern const TSLanguage *tree_sitter_verilog(void);
extern const TSLanguage *tree_sitter_elisp(void);
extern const TSLanguage *tree_sitter_json(void);
extern const TSLanguage *tree_sitter_xml(void);
extern const TSLanguage *tree_sitter_markdown(void);
extern const TSLanguage *tree_sitter_make(void);
extern const TSLanguage *tree_sitter_cmake(void);
extern const TSLanguage *tree_sitter_proto(void);
extern const TSLanguage *tree_sitter_graphql(void);
extern const TSLanguage *tree_sitter_vue(void);
extern const TSLanguage *tree_sitter_svelte(void);
extern const TSLanguage *tree_sitter_meson(void);
extern const TSLanguage *tree_sitter_glsl(void);
extern const TSLanguage *tree_sitter_ini(void);
// Scientific/math languages
extern const TSLanguage *tree_sitter_matlab(void);
extern const TSLanguage *tree_sitter_lean(void);
extern const TSLanguage *tree_sitter_form(void);
extern const TSLanguage *tree_sitter_magma(void);
extern const TSLanguage *tree_sitter_wolfram(void);

// New languages
extern const TSLanguage *tree_sitter_solidity(void);
extern const TSLanguage *tree_sitter_typst(void);
extern const TSLanguage *tree_sitter_gdscript(void);
extern const TSLanguage *tree_sitter_gleam(void);
extern const TSLanguage *tree_sitter_powershell(void);
extern const TSLanguage *tree_sitter_pascal(void);
extern const TSLanguage *tree_sitter_d(void);
extern const TSLanguage *tree_sitter_nim(void);
extern const TSLanguage *tree_sitter_scheme(void);
extern const TSLanguage *tree_sitter_fennel(void);
extern const TSLanguage *tree_sitter_fish(void);
extern const TSLanguage *tree_sitter_awk(void);
extern const TSLanguage *tree_sitter_zsh(void);
extern const TSLanguage *tree_sitter_tcl(void);
extern const TSLanguage *tree_sitter_ada(void);
extern const TSLanguage *tree_sitter_agda(void);
extern const TSLanguage *tree_sitter_racket(void);
extern const TSLanguage *tree_sitter_odin(void);
extern const TSLanguage *tree_sitter_rescript(void);
extern const TSLanguage *tree_sitter_purescript(void);
extern const TSLanguage *tree_sitter_nickel(void);
extern const TSLanguage *tree_sitter_crystal(void);
extern const TSLanguage *tree_sitter_teal(void);
extern const TSLanguage *tree_sitter_hare(void);
extern const TSLanguage *tree_sitter_pony(void);
extern const TSLanguage *tree_sitter_luau(void);
extern const TSLanguage *tree_sitter_janet_simple(void);
extern const TSLanguage *tree_sitter_sway(void);
extern const TSLanguage *tree_sitter_nasm(void);
extern const TSLanguage *tree_sitter_asm(void);
extern const TSLanguage *tree_sitter_astro(void);
extern const TSLanguage *tree_sitter_blade(void);
extern const TSLanguage *tree_sitter_just(void);
extern const TSLanguage *tree_sitter_gotmpl(void);
extern const TSLanguage *tree_sitter_templ(void);
extern const TSLanguage *tree_sitter_liquid(void);
extern const TSLanguage *tree_sitter_jinja2(void);
extern const TSLanguage *tree_sitter_prisma(void);
extern const TSLanguage *tree_sitter_hyprlang(void);
extern const TSLanguage *tree_sitter_dotenv(void);
extern const TSLanguage *tree_sitter_diff(void);
extern const TSLanguage *tree_sitter_wgsl(void);
extern const TSLanguage *tree_sitter_kdl(void);
extern const TSLanguage *tree_sitter_json5(void);
extern const TSLanguage *tree_sitter_jsonnet(void);
extern const TSLanguage *tree_sitter_ron(void);
extern const TSLanguage *tree_sitter_thrift(void);
extern const TSLanguage *tree_sitter_capnp(void);
extern const TSLanguage *tree_sitter_properties(void);
extern const TSLanguage *tree_sitter_ssh_config(void);
extern const TSLanguage *tree_sitter_bibtex(void);
extern const TSLanguage *tree_sitter_starlark(void);
extern const TSLanguage *tree_sitter_bicep(void);
extern const TSLanguage *tree_sitter_csv(void);
extern const TSLanguage *tree_sitter_requirements(void);
extern const TSLanguage *tree_sitter_hlsl(void);
extern const TSLanguage *tree_sitter_vhdl(void);
extern const TSLanguage *tree_sitter_systemverilog(void);
extern const TSLanguage *tree_sitter_devicetree(void);
extern const TSLanguage *tree_sitter_linkerscript(void);
extern const TSLanguage *tree_sitter_gn(void);
extern const TSLanguage *tree_sitter_kconfig(void);
extern const TSLanguage *tree_sitter_bitbake(void);
extern const TSLanguage *tree_sitter_smali(void);
extern const TSLanguage *tree_sitter_tablegen(void);
extern const TSLanguage *tree_sitter_ispc(void);
extern const TSLanguage *tree_sitter_cairo(void);
extern const TSLanguage *tree_sitter_move(void);
extern const TSLanguage *tree_sitter_squirrel(void);
extern const TSLanguage *tree_sitter_func(void);
extern const TSLanguage *tree_sitter_regex(void);
extern const TSLanguage *tree_sitter_jsdoc(void);
extern const TSLanguage *tree_sitter_rst(void);
extern const TSLanguage *tree_sitter_beancount(void);
extern const TSLanguage *tree_sitter_mermaid(void);
extern const TSLanguage *tree_sitter_puppet(void);
extern const TSLanguage *tree_sitter_po(void);
extern const TSLanguage *tree_sitter_gitattributes(void);
extern const TSLanguage *tree_sitter_gitignore(void);
extern const TSLanguage *tree_sitter_slang(void);
extern const TSLanguage *tree_sitter_llvm(void);
extern const TSLanguage *tree_sitter_smithy(void);
extern const TSLanguage *tree_sitter_wit(void);
extern const TSLanguage *tree_sitter_tlaplus(void);
extern const TSLanguage *tree_sitter_pkl(void);
extern const TSLanguage *tree_sitter_gomod(void);
extern const TSLanguage *tree_sitter_apex(void);
extern const TSLanguage *tree_sitter_soql(void);
extern const TSLanguage *tree_sitter_sosl(void);

// -- Empty sentinel --
static const char *empty_types[] = {NULL};

// ==================== GO ====================
static const char *go_func_types[] = {"function_declaration", "method_declaration", "method_elem",
                                      NULL};
static const char *go_class_types[] = {"type_spec", "type_alias", NULL};
static const char *go_field_types[] = {"field_declaration", NULL};
static const char *go_module_types[] = {"source_file", NULL};
static const char *go_call_types[] = {"call_expression", NULL};
static const char *go_import_types[] = {"import_declaration", NULL};
static const char *go_branch_types[] = {"if_statement",
                                        "for_statement",
                                        "switch_expression",
                                        "select_statement",
                                        "case_clause",
                                        "default_clause",
                                        NULL};
static const char *go_var_types[] = {"var_declaration", "const_declaration", NULL};
static const char *go_assign_types[] = {"assignment_statement", "short_var_declaration", NULL};

// ==================== PYTHON ====================
static const char *py_func_types[] = {"function_definition", NULL};
static const char *py_class_types[] = {"class_definition", NULL};
static const char *py_module_types[] = {"module", NULL};
static const char *py_call_types[] = {"call", "with_statement", NULL};
static const char *py_import_types[] = {"import_statement", NULL};
static const char *py_import_from_types[] = {"import_from_statement", NULL};
static const char *py_branch_types[] = {
    "if_statement",  "for_statement",  "while_statement", "try_statement",
    "except_clause", "with_statement", "elif_clause",     NULL};
static const char *py_var_types[] = {"assignment", "augmented_assignment", NULL};
static const char *py_throw_types[] = {"raise_statement", NULL};
static const char *py_decorator_types[] = {"decorator", NULL};

// ==================== JAVASCRIPT ====================
static const char *js_func_types[] = {"function_declaration", "generator_function_declaration",
                                      "function_expression",  "arrow_function",
                                      "method_definition",    NULL};
static const char *js_class_types[] = {"class_declaration", "class", NULL};
static const char *js_module_types[] = {"program", NULL};
static const char *js_call_types[] = {"call_expression", NULL};
static const char *js_import_types[] = {"import_statement", "lexical_declaration",
                                        "export_statement", NULL};
static const char *js_branch_types[] = {"if_statement",    "for_statement",    "for_in_statement",
                                        "while_statement", "switch_statement", "case_clause",
                                        "try_statement",   "catch_clause",     NULL};
static const char *js_var_types[] = {"lexical_declaration", "variable_declaration", NULL};
static const char *js_throw_types[] = {"throw_statement", NULL};

// ==================== TYPESCRIPT ====================
static const char *ts_func_types[] = {"function_declaration",
                                      "generator_function_declaration",
                                      "function_expression",
                                      "arrow_function",
                                      "method_definition",
                                      "function_signature",
                                      NULL};
static const char *ts_class_types[] = {"class_declaration",
                                       "class",
                                       "abstract_class_declaration",
                                       "enum_declaration",
                                       "interface_declaration",
                                       "type_alias_declaration",
                                       "internal_module",
                                       NULL};
static const char *ts_decorator_types[] = {"decorator", NULL};

// ==================== RUST ====================
static const char *rust_func_types[] = {"function_item", "function_signature_item",
                                        "closure_expression", NULL};
static const char *rust_class_types[] = {"struct_item", "enum_item", "union_item",
                                         "trait_item",  "type_item", NULL};
static const char *rust_field_types[] = {"field_declaration", NULL};
static const char *rust_module_types[] = {"source_file", "mod_item", NULL};
static const char *rust_call_types[] = {"call_expression", "macro_invocation", NULL};
static const char *rust_import_types[] = {"use_declaration", "extern_crate_declaration", NULL};
static const char *rust_import_from_types[] = {"use_declaration", NULL};
static const char *rust_branch_types[] = {"if_expression",
                                          "for_expression",
                                          "while_expression",
                                          "loop_expression",
                                          "match_expression",
                                          "match_arm",
                                          NULL};
static const char *rust_var_types[] = {"static_item", "const_item", NULL};
static const char *rust_assign_types[] = {"assignment_expression", "compound_assignment_expr",
                                          NULL};
static const char *rust_decorator_types[] = {"attribute_item", NULL};

// ==================== JAVA ====================
static const char *java_func_types[] = {"method_declaration", "constructor_declaration", NULL};
static const char *java_class_types[] = {"class_declaration",  "interface_declaration",
                                         "enum_declaration",   "annotation_type_declaration",
                                         "record_declaration", NULL};
static const char *java_field_types[] = {"field_declaration", NULL};
static const char *java_module_types[] = {"program", NULL};
static const char *java_call_types[] = {"method_invocation", NULL};
static const char *java_import_types[] = {"import_declaration", NULL};
static const char *java_branch_types[] = {
    "if_statement",    "for_statement",     "enhanced_for_statement",
    "while_statement", "switch_expression", "switch_block_statement_group",
    "try_statement",   "catch_clause",      NULL};
static const char *java_var_types[] = {"field_declaration", "local_variable_declaration", NULL};
static const char *java_assign_types[] = {"assignment_expression", NULL};
static const char *java_throw_types[] = {"throw_statement", NULL};
static const char *java_decorator_types[] = {"marker_annotation", "annotation", NULL};

// ==================== C++ ====================
static const char *cpp_func_types[] = {"function_definition", "declaration",
                                       "field_declaration",   "template_declaration",
                                       "lambda_expression",   NULL};
static const char *cpp_class_types[] = {"class_specifier", "struct_specifier", "union_specifier",
                                        "enum_specifier", NULL};
static const char *cpp_field_types[] = {"field_declaration", NULL};
static const char *cpp_module_types[] = {"translation_unit", "namespace_definition",
                                         "linkage_specification", "declaration", NULL};
static const char *cpp_call_types[] = {
    "call_expression",  "field_expression",  "subscript_expression",
    "new_expression",   "delete_expression", "binary_expression",
    "unary_expression", "update_expression", NULL};
static const char *cpp_import_types[] = {"preproc_include", "template_function", "declaration",
                                         NULL};
static const char *cpp_branch_types[] = {"if_statement",    "for_statement",    "for_range_loop",
                                         "while_statement", "switch_statement", "case_statement",
                                         "try_statement",   "catch_clause",     NULL};
static const char *cpp_var_types[] = {"declaration", NULL};
static const char *cpp_assign_types[] = {"assignment_expression", NULL};
static const char *cpp_throw_types[] = {"throw_statement", NULL};

// ==================== C# ====================
static const char *cs_func_types[] = {"destructor_declaration",      "local_function_statement",
                                      "function_pointer_type",       "constructor_declaration",
                                      "anonymous_method_expression", "lambda_expression",
                                      "method_declaration",          NULL};
static const char *cs_class_types[] = {"class_declaration", "struct_declaration",
                                       "enum_declaration", "interface_declaration", NULL};
static const char *cs_module_types[] = {"compilation_unit", NULL};
static const char *cs_call_types[] = {"invocation_expression", NULL};
static const char *cs_import_types[] = {"using_directive", NULL};
static const char *cs_branch_types[] = {"if_statement",    "for_statement",    "foreach_statement",
                                        "while_statement", "switch_statement", "case_switch_label",
                                        "try_statement",   "catch_clause",     NULL};
static const char *cs_var_types[] = {"field_declaration", "local_declaration_statement", NULL};
static const char *cs_assign_types[] = {"assignment_expression", NULL};
static const char *cs_throw_types[] = {"throw_statement", "throw_expression", NULL};
static const char *cs_decorator_types[] = {"attribute", NULL};

// ==================== PHP ====================
static const char *php_func_types[] = {"function_static_declaration", "anonymous_function",
                                       "function_definition",         "arrow_function",
                                       "method_declaration",          NULL};
static const char *php_class_types[] = {"trait_declaration", "enum_declaration",
                                        "interface_declaration", "class_declaration", NULL};
static const char *php_module_types[] = {"program", NULL};
static const char *php_call_types[] = {"member_call_expression", "scoped_call_expression",
                                       "function_call_expression",
                                       "nullsafe_member_call_expression", NULL};
static const char *php_branch_types[] = {"if_statement",    "for_statement",    "foreach_statement",
                                         "while_statement", "switch_statement", "case_statement",
                                         "try_statement",   "catch_clause",     NULL};
static const char *php_var_types[] = {"expression_statement", NULL};
static const char *php_assign_types[] = {"assignment_expression", NULL};
static const char *php_throw_types[] = {"throw_expression", NULL};
static const char *php_decorator_types[] = {"attribute_group", NULL};

// ==================== LUA ====================
static const char *lua_func_types[] = {"function_declaration", "function_definition", NULL};
static const char *lua_module_types[] = {"chunk", NULL};
static const char *lua_call_types[] = {"function_call", NULL};
static const char *lua_import_types[] = {"function_call", NULL};
static const char *lua_branch_types[] = {"if_statement",    "for_statement",    "for_in_statement",
                                         "while_statement", "repeat_statement", NULL};
static const char *lua_var_types[] = {"variable_declaration", NULL};
static const char *lua_assign_types[] = {"assignment_statement", NULL};

// ==================== SCALA ====================
static const char *scala_func_types[] = {"function_definition", "function_declaration", NULL};
static const char *scala_class_types[] = {"class_definition", "object_definition",
                                          "trait_definition", NULL};
static const char *scala_module_types[] = {"compilation_unit", NULL};
static const char *scala_call_types[] = {"call_expression", "generic_function", "field_expression",
                                         "infix_expression", NULL};
static const char *scala_import_types[] = {"import_declaration", NULL};
static const char *scala_branch_types[] = {
    "if_expression", "for_expression", "while_expression", "match_expression",
    "case_clause",   "try_expression", "catch_clause",     NULL};
static const char *scala_var_types[] = {"val_definition", "var_definition", "val_declaration",
                                        "var_declaration", NULL};
static const char *scala_assign_types[] = {"assignment_expression", NULL};
static const char *scala_throw_types[] = {"throw_expression", NULL};

// ==================== KOTLIN ====================
static const char *kotlin_func_types[] = {"function_declaration", "secondary_constructor",
                                          "anonymous_function", NULL};
static const char *kotlin_class_types[] = {"class_declaration", "object_declaration",
                                           "companion_object", NULL};
static const char *kotlin_module_types[] = {"source_file", NULL};
static const char *kotlin_call_types[] = {"call_expression", "navigation_expression", NULL};
static const char *kotlin_import_types[] = {"import", NULL};
static const char *kotlin_branch_types[] = {
    "if_expression", "for_statement",  "while_statement", "when_expression",
    "when_entry",    "try_expression", "catch_block",     NULL};
static const char *kotlin_var_types[] = {"property_declaration", NULL};
static const char *kotlin_assign_types[] = {"assignment", "directly_assignable_expression", NULL};
static const char *kotlin_throw_types[] = {"throw_expression", NULL};
static const char *kotlin_decorator_types[] = {"annotation", NULL};

// ==================== RUBY ====================
static const char *ruby_func_types[] = {"method", "singleton_method", NULL};
static const char *ruby_class_types[] = {"class", "module", NULL};
static const char *ruby_module_types[] = {"program", NULL};
static const char *ruby_call_types[] = {"call", "command_call", NULL};
static const char *ruby_import_types[] = {"call", NULL};
static const char *ruby_branch_types[] = {"if",   "unless", "while",  "until", "for",
                                          "case", "when",   "rescue", "elsif", NULL};
static const char *ruby_var_types[] = {"assignment", NULL};
static const char *ruby_assign_types[] = {"assignment", "operator_assignment", NULL};

// ==================== C ====================
static const char *c_func_types[] = {"function_definition", NULL};
static const char *c_class_types[] = {"struct_specifier", "enum_specifier", "union_specifier",
                                      NULL};
static const char *c_field_types[] = {"field_declaration", NULL};
static const char *c_module_types[] = {"translation_unit", NULL};
static const char *c_call_types[] = {"call_expression", NULL};
static const char *c_import_types[] = {"preproc_include", NULL};
static const char *c_branch_types[] = {"if_statement",
                                       "for_statement",
                                       "while_statement",
                                       "do_statement",
                                       "switch_statement",
                                       "case_statement",
                                       NULL};
static const char *c_var_types[] = {"declaration", NULL};
static const char *c_assign_types[] = {"assignment_expression", NULL};

// ==================== BASH ====================
static const char *bash_func_types[] = {"function_definition", NULL};
static const char *bash_module_types[] = {"program", NULL};
static const char *bash_call_types[] = {"command", NULL};
static const char *bash_import_types[] = {"command", NULL};
static const char *bash_branch_types[] = {"if_statement",   "while_statement", "for_statement",
                                          "case_statement", "elif_clause",     NULL};
static const char *bash_var_types[] = {"variable_assignment", NULL};

// ==================== ZIG ====================
static const char *zig_func_types[] = {"function_declaration", "test_declaration", NULL};
static const char *zig_class_types[] = {"struct_declaration", "enum_declaration",
                                        "union_declaration", NULL};
static const char *zig_field_types[] = {"container_field", NULL};
static const char *zig_module_types[] = {"source_file", NULL};
static const char *zig_call_types[] = {"call_expression", "builtin_function", NULL};
static const char *zig_import_types[] = {"builtin_function", NULL};
static const char *zig_branch_types[] = {"if_statement", "for_statement", "while_statement",
                                         "switch_expression", NULL};
static const char *zig_var_types[] = {"variable_declaration", NULL};
static const char *zig_assign_types[] = {"assignment_expression", NULL};

// ==================== ELIXIR ====================
static const char *elixir_func_types[] = {"call", NULL};
static const char *elixir_module_types[] = {"source", NULL};
static const char *elixir_call_types[] = {"call", "dot", "binary_operator", NULL};
static const char *elixir_import_types[] = {"call", NULL};
static const char *elixir_branch_types[] = {"call", NULL};
static const char *elixir_var_types[] = {"binary_operator", NULL};

// ==================== HASKELL ====================
static const char *haskell_func_types[] = {"function", "signature", NULL};
static const char *haskell_class_types[] = {"class", "data_type", "newtype", NULL};
static const char *haskell_module_types[] = {"haskell", NULL};
static const char *haskell_call_types[] = {"infix", "apply", NULL};
static const char *haskell_import_types[] = {"import", NULL};
static const char *haskell_branch_types[] = {"match", "guards",  "if", "case",
                                             "do",    "boolean", NULL};
static const char *haskell_var_types[] = {"function", NULL};

// ==================== OCAML ====================
static const char *ocaml_func_types[] = {"value_definition", NULL};
static const char *ocaml_class_types[] = {"type_definition", "class_definition",
                                          "module_definition", NULL};
static const char *ocaml_module_types[] = {"compilation_unit", NULL};
static const char *ocaml_call_types[] = {"application_expression", "infix_expression", NULL};
static const char *ocaml_import_types[] = {"open_module", NULL};
static const char *ocaml_branch_types[] = {"match_expression", "if_expression", "match_case", NULL};
static const char *ocaml_var_types[] = {"value_definition", NULL};

// ==================== OBJECTIVE-C ====================
static const char *objc_func_types[] = {"function_definition", "method_definition", NULL};
static const char *objc_class_types[] = {"class_interface", "class_implementation",
                                         "protocol_declaration", NULL};
static const char *objc_field_types[] = {"property_declaration", NULL};
static const char *objc_module_types[] = {"translation_unit", NULL};
static const char *objc_call_types[] = {"call_expression", "message_expression", NULL};
static const char *objc_import_types[] = {"preproc_import", NULL};
static const char *objc_branch_types[] = {"if_statement", "for_statement", "while_statement",
                                          "switch_statement", NULL};
static const char *objc_var_types[] = {"declaration", NULL};
static const char *objc_assign_types[] = {"assignment_expression", NULL};

// ==================== SWIFT ====================
static const char *swift_func_types[] = {"function_declaration", NULL};
static const char *swift_class_types[] = {"class_declaration", "protocol_declaration",
                                          "struct_declaration", "enum_declaration", NULL};
static const char *swift_field_types[] = {"property_declaration", NULL};
static const char *swift_module_types[] = {"source_file", NULL};
static const char *swift_call_types[] = {"call_expression", "constructor_expression", NULL};
static const char *swift_import_types[] = {"import_declaration", NULL};
static const char *swift_branch_types[] = {"if_statement",    "guard_statement",  "for_statement",
                                           "while_statement", "switch_statement", NULL};
static const char *swift_var_types[] = {"property_declaration", NULL};
static const char *swift_assign_types[] = {"assignment", NULL};
static const char *swift_throw_types[] = {"throw_statement", NULL};
static const char *swift_decorator_types[] = {"attribute", NULL};

// ==================== DART ====================
static const char *dart_func_types[] = {"function_signature", "method_signature", NULL};
static const char *dart_class_types[] = {"class_definition", "enum_declaration",
                                         "mixin_declaration", NULL};
static const char *dart_field_types[] = {"declaration", NULL};
static const char *dart_module_types[] = {"program", NULL};
static const char *dart_call_types[] = {"selector", NULL};
static const char *dart_import_types[] = {"import_or_export", NULL};
static const char *dart_branch_types[] = {"if_statement", "for_statement", "while_statement",
                                          "switch_statement", NULL};
static const char *dart_var_types[] = {"declaration", NULL};
static const char *dart_assign_types[] = {"assignment_expression", NULL};
static const char *dart_throw_types[] = {"throw_expression", NULL};
static const char *dart_decorator_types[] = {"annotation", NULL};

// ==================== PERL ====================
static const char *perl_func_types[] = {"subroutine_declaration_statement", NULL};
static const char *perl_module_types[] = {"source_file", NULL};
static const char *perl_call_types[] = {"ambiguous_function_call_expression",
                                        "function_call_expression", "func1op_call_expression",
                                        NULL};
static const char *perl_import_types[] = {"use_statement", "require_statement", NULL};
static const char *perl_branch_types[] = {"if_statement",      "unless_statement", "for_statement",
                                          "foreach_statement", "while_statement",  NULL};
static const char *perl_var_types[] = {"variable_declaration", "expression_statement", NULL};
static const char *perl_assign_types[] = {"assignment_expression", NULL};

// ==================== GROOVY ====================
static const char *groovy_func_types[] = {"function_definition", NULL};
static const char *groovy_class_types[] = {"class_definition", NULL};
static const char *groovy_module_types[] = {"source_file", NULL};
static const char *groovy_call_types[] = {"function_call", "juxt_function_call", NULL};
static const char *groovy_import_types[] = {"groovy_import", NULL};
static const char *groovy_branch_types[] = {"if_statement", "for_statement", "while_statement",
                                            "switch_statement", NULL};
static const char *groovy_var_types[] = {"declaration", NULL};
static const char *groovy_assign_types[] = {"assignment", NULL};
static const char *groovy_throw_types[] = {"throw_statement", NULL};
static const char *groovy_decorator_types[] = {"annotation", NULL};

// ==================== ERLANG ====================
static const char *erlang_func_types[] = {"function_clause", NULL};
static const char *erlang_module_types[] = {"source_file", NULL};
static const char *erlang_call_types[] = {"call", NULL};
static const char *erlang_import_types[] = {"module_attribute", NULL};
static const char *erlang_branch_types[] = {"if_expression", "case_expression",
                                            "receive_expression", NULL};
static const char *erlang_var_types[] = {"pp_define", "record_decl", NULL};
static const char *erlang_assign_types[] = {"match_expression", NULL};
static const char *erlang_throw_types[] = {"call", NULL};

// ==================== R ====================
static const char *r_func_types[] = {"function_definition", NULL};
static const char *r_module_types[] = {"program", NULL};
static const char *r_call_types[] = {"call", NULL};
static const char *r_import_types[] = {"call", NULL};
static const char *r_branch_types[] = {"if_statement", "for_statement", "while_statement", NULL};
static const char *r_var_types[] = {"binary_operator", NULL};

// ==================== HTML ====================
static const char *html_module_types[] = {"document", NULL};

// ==================== CSS ====================
static const char *css_module_types[] = {"stylesheet", NULL};
static const char *css_import_types[] = {"import_statement", NULL};

// ==================== SCSS ====================
static const char *scss_func_types[] = {"mixin_statement", "function_statement", NULL};
static const char *scss_module_types[] = {"stylesheet", NULL};
static const char *scss_call_types[] = {"call_expression", NULL};
static const char *scss_import_types[] = {"import_statement", "use_statement", NULL};
static const char *scss_branch_types[] = {"if_statement", NULL};
static const char *scss_var_types[] = {"declaration", NULL};

// ==================== YAML ====================
static const char *yaml_module_types[] = {"stream", NULL};
static const char *yaml_var_types[] = {"block_mapping_pair", NULL};

// ==================== TOML ====================
static const char *toml_module_types[] = {"document", NULL};
static const char *toml_class_types[] = {"table", "table_array_element", NULL};
static const char *toml_var_types[] = {"pair", NULL};

// ==================== HCL ====================
static const char *hcl_class_types[] = {"block", NULL};
static const char *hcl_module_types[] = {"config_file", NULL};
static const char *hcl_call_types[] = {"function_call", NULL};
static const char *hcl_var_types[] = {"attribute", NULL};

// ==================== SQL ====================
static const char *sql_func_types[] = {"create_function", NULL};
static const char *sql_field_types[] = {"column_definition", NULL};
static const char *sql_module_types[] = {"program", NULL};
static const char *sql_call_types[] = {"function_call", "invocation", NULL};
static const char *sql_branch_types[] = {"if_statement", "case_expression", NULL};
static const char *sql_var_types[] = {"create_table", "create_view", NULL};

// ==================== DOCKERFILE ====================
static const char *dockerfile_module_types[] = {"source_file", NULL};
static const char *dockerfile_var_types[] = {"env_instruction", "arg_instruction", NULL};

// ==================== ENV ACCESS ====================
static const char *go_env_funcs[] = {"os.Getenv", "os.LookupEnv", NULL};
static const char *py_env_funcs[] = {"os.getenv", "os.environ.get", NULL};
static const char *py_env_members[] = {"os.environ", NULL};
static const char *js_env_members[] = {"process.env", NULL};
static const char *ts_env_members[] = {"process.env", NULL};
static const char *rust_env_funcs[] = {"env::var", "std::env::var", NULL};
static const char *java_env_funcs[] = {"System.getenv", "System.getProperty", NULL};
static const char *cpp_env_funcs[] = {"getenv", "std::getenv", NULL};
static const char *cs_env_funcs[] = {"Environment.GetEnvironmentVariable", NULL};
static const char *php_env_funcs[] = {"getenv", "env", NULL};
static const char *lua_env_funcs[] = {"os.getenv", NULL};
static const char *scala_env_funcs[] = {"sys.env", "System.getenv", "System.getProperty", NULL};
static const char *kotlin_env_funcs[] = {"System.getenv", "System.getProperty", NULL};
static const char *ruby_env_members[] = {"ENV", NULL};
static const char *c_env_funcs[] = {"getenv", NULL};
static const char *zig_env_funcs[] = {"std.os.getenv", NULL};
static const char *elixir_env_funcs[] = {"System.get_env", NULL};
static const char *haskell_env_funcs[] = {"lookupEnv", "getEnv", NULL};
static const char *ocaml_env_funcs[] = {"Sys.getenv", NULL};
static const char *r_env_funcs[] = {"Sys.getenv", NULL};
static const char *perl_env_funcs[] = {"$ENV", NULL};

// ==================== CLOJURE ====================
static const char *clojure_module_types[] = {"source", NULL};
static const char *clojure_call_types[] = {"list_lit", NULL};

// ==================== F# ====================
static const char *fsharp_func_types[] = {"function_declaration", "value_declaration", NULL};
static const char *fsharp_class_types[] = {"type_definition", "exception_definition", NULL};
static const char *fsharp_module_types[] = {"file", NULL};
static const char *fsharp_call_types[] = {"application_expression", "dot_expression", NULL};
static const char *fsharp_import_types[] = {"import_decl", "open_expression", NULL};
static const char *fsharp_branch_types[] = {"if_expression",    "for_expression",
                                            "while_expression", "match_expression",
                                            "elif_expression",  NULL};
static const char *fsharp_var_types[] = {"value_declaration", NULL};

// ==================== JULIA ====================
static const char *julia_func_types[] = {"function_definition", "short_function_definition", NULL};
static const char *julia_class_types[] = {"struct_definition", "abstract_definition", NULL};
static const char *julia_module_types[] = {"source_file", NULL};
static const char *julia_call_types[] = {"call_expression", "broadcast_call_expression", NULL};
static const char *julia_import_types[] = {"import_statement", "using_statement", NULL};
static const char *julia_branch_types[] = {"if_statement", "for_statement", "while_statement",
                                           "try_statement", NULL};
static const char *julia_var_types[] = {"const_statement", "assignment", NULL};
static const char *julia_assign_types[] = {"assignment", "compound_assignment_expression", NULL};
static const char *julia_throw_types[] = {"throw_statement", NULL};

// ==================== VIM SCRIPT ====================
static const char *vim_func_types[] = {"function_definition", NULL};
static const char *vim_module_types[] = {"script_file", NULL};
static const char *vim_call_types[] = {"call_expression", NULL};
static const char *vim_branch_types[] = {"if_statement", "for_statement", "while_statement",
                                         "try_statement", NULL};
static const char *vim_var_types[] = {"let_statement", NULL};

// ==================== NIX ====================
static const char *nix_func_types[] = {"function_expression", NULL};
static const char *nix_module_types[] = {"source_expression", NULL};
static const char *nix_call_types[] = {"apply_expression", NULL};
static const char *nix_branch_types[] = {"if_expression", NULL};
static const char *nix_var_types[] = {"binding", NULL};

// ==================== COMMON LISP ====================
static const char *commonlisp_func_types[] = {"defun", NULL};
static const char *commonlisp_module_types[] = {"source", NULL};
static const char *commonlisp_call_types[] = {"list_lit", NULL};

// ==================== ELM ====================
static const char *elm_func_types[] = {"value_declaration", "function_declaration", NULL};
static const char *elm_class_types[] = {"type_declaration", "type_alias_declaration", NULL};
static const char *elm_module_types[] = {"file", NULL};
static const char *elm_call_types[] = {"function_call", NULL};
static const char *elm_import_types[] = {"import", NULL};
static const char *elm_branch_types[] = {"case_of_expr", "if_else_expr", NULL};

// ==================== FORTRAN ====================
static const char *fortran_func_types[] = {"function", "subroutine", NULL};
static const char *fortran_class_types[] = {"derived_type_definition", NULL};
static const char *fortran_module_types[] = {"translation_unit", NULL};
static const char *fortran_call_types[] = {"call_expression", "keyword_argument", NULL};
static const char *fortran_import_types[] = {"use_statement", "include_statement", NULL};
static const char *fortran_branch_types[] = {"if_statement", "do_loop_statement", "where_statement",
                                             "select_case_statement", NULL};
static const char *fortran_var_types[] = {"variable_declaration", NULL};
static const char *fortran_assign_types[] = {"assignment_statement", NULL};

// ==================== CUDA ====================
// CUDA extends C++, reuse cpp types (same grammar family)

// ==================== COBOL ====================
static const char *cobol_func_types[] = {"program_definition", NULL};
static const char *cobol_module_types[] = {"source_file", NULL};
static const char *cobol_call_types[] = {"call_statement", NULL};
static const char *cobol_branch_types[] = {"if_statement", "evaluate_statement",
                                           "perform_statement", NULL};
static const char *cobol_var_types[] = {"data_description_entry", NULL};

// ==================== VERILOG ====================
static const char *verilog_func_types[] = {"function_declaration", "task_declaration", NULL};
static const char *verilog_class_types[] = {"module_declaration", "class_declaration",
                                            "interface_declaration", NULL};
static const char *verilog_module_types[] = {"source_file", NULL};
static const char *verilog_call_types[] = {"system_tf_call", "subroutine_call", NULL};
static const char *verilog_branch_types[] = {"conditional_statement", "case_statement",
                                             "loop_statement", NULL};
static const char *verilog_var_types[] = {"net_declaration", "data_declaration", NULL};
static const char *verilog_assign_types[] = {"blocking_assignment", "nonblocking_assignment", NULL};

// ==================== EMACS LISP ====================
static const char *elisp_func_types[] = {"function_definition", "macro_definition", NULL};
static const char *elisp_module_types[] = {"source_file", NULL};
static const char *elisp_call_types[] = {"list", NULL};

// ==================== JSON ====================
static const char *json_module_types[] = {"document", NULL};
static const char *json_var_types[] = {"pair", NULL};

// ==================== XML ====================
static const char *xml_module_types[] = {"document", NULL};
static const char *xml_class_types[] = {"element", NULL};

// ==================== MARKDOWN ====================
static const char *markdown_module_types[] = {"document", NULL};
static const char *markdown_class_types[] = {"atx_heading", "setext_heading", NULL};

// ==================== MAKEFILE ====================
static const char *makefile_func_types[] = {"rule", NULL};
static const char *makefile_module_types[] = {"makefile", NULL};
static const char *makefile_call_types[] = {"function_call", NULL};
static const char *makefile_import_types[] = {"include_directive", NULL};
static const char *makefile_var_types[] = {"variable_assignment", NULL};

// ==================== CMAKE ====================
static const char *cmake_module_types[] = {"source_file", NULL};
static const char *cmake_call_types[] = {"normal_command", NULL};

// ==================== PROTOBUF ====================
static const char *protobuf_class_types[] = {"message", "enum", NULL};
static const char *protobuf_module_types[] = {"source_file", NULL};
static const char *protobuf_field_types[] = {"field", "map_field", "oneof_field", NULL};
static const char *protobuf_import_types[] = {"import", NULL};

// ==================== GRAPHQL ====================
static const char *graphql_class_types[] = {"object_type_definition",
                                            "input_object_type_definition",
                                            "enum_type_definition",
                                            "interface_type_definition",
                                            "union_type_definition",
                                            "scalar_type_definition",
                                            NULL};
static const char *graphql_module_types[] = {"document", NULL};
static const char *graphql_field_types[] = {"field_definition", "input_value_definition", NULL};

// ==================== VUE ====================
static const char *vue_module_types[] = {"document", NULL};

// ==================== SVELTE ====================
static const char *svelte_module_types[] = {"document", NULL};
static const char *svelte_branch_types[] = {"if_statement", "each_statement", "await_statement",
                                            NULL};

// ==================== MESON ====================
static const char *meson_func_types[] = {"function_expression", NULL};
static const char *meson_module_types[] = {"source_file", NULL};
static const char *meson_call_types[] = {"function_expression", NULL};
static const char *meson_branch_types[] = {"if_statement", "foreach_statement", NULL};
static const char *meson_var_types[] = {"assignment_statement", NULL};

// ==================== GLSL ====================
// GLSL extends C, reuse c types (same grammar family)

// ==================== INI ====================
static const char *ini_module_types[] = {"document", NULL};
static const char *ini_class_types[] = {"section", NULL};
static const char *ini_var_types[] = {"setting", NULL};

// ==================== MATLAB ====================
static const char *matlab_func_types[] = {"function_definition", NULL};
static const char *matlab_class_types[] = {"class_definition", NULL};
static const char *matlab_module_types[] = {"source_file", NULL};
static const char *matlab_call_types[] = {"function_call", "command", NULL};
static const char *matlab_branch_types[] = {"if_statement",     "for_statement", "while_statement",
                                            "switch_statement", "try_statement", NULL};
static const char *matlab_var_types[] = {"assignment", NULL};

// ==================== LEAN ====================
static const char *lean_func_types[] = {"def", "theorem", "instance", "abbrev", NULL};
static const char *lean_class_types[] = {"structure", "class_inductive", "inductive", NULL};
static const char *lean_module_types[] = {"module", NULL};
static const char *lean_call_types[] = {"apply", NULL};
static const char *lean_import_types[] = {"import", NULL};
static const char *lean_branch_types[] = {"if", "match", "do", NULL};

// ==================== FORM ====================
static const char *form_func_types[] = {"procedure_definition", NULL};
static const char *form_module_types[] = {"source_file", NULL};
static const char *form_call_types[] = {"call_statement", NULL};
static const char *form_import_types[] = {"include_directive", NULL};
static const char *form_branch_types[] = {"if_statement", "repeat_statement", "do_loop", NULL};
static const char *form_var_types[] = {"declaration_statement", NULL};
static const char *form_assign_types[] = {"substitution_statement", NULL};

// ==================== MAGMA ====================
static const char *magma_func_types[] = {"function_definition", "procedure_definition",
                                         "intrinsic_definition", NULL};
static const char *magma_module_types[] = {"source_file", NULL};
static const char *magma_call_types[] = {"call_expression", NULL};
static const char *magma_import_types[] = {"load_statement", NULL};
static const char *magma_branch_types[] = {"if_statement",     "for_statement",  "while_statement",
                                           "repeat_statement", "case_statement", NULL};
static const char *magma_var_types[] = {"assignment_statement", NULL};

// ==================== WOLFRAM ====================
static const char *wolfram_func_types[] = {"set_delayed_top", "set_top", "set_delayed", "set",
                                           NULL};
static const char *wolfram_module_types[] = {"source_file", NULL};
static const char *wolfram_call_types[] = {"apply", NULL};
static const char *wolfram_import_types[] = {"get_top", NULL};

// ==================== NEW LANG ENV ACCESS ====================
static const char *julia_env_funcs[] = {"ENV", NULL};
static const char *nix_env_funcs[] = {"builtins.getEnv", NULL};
static const char *fortran_env_funcs[] = {"get_environment_variable", NULL};
static const char *fsharp_env_funcs[] = {"Environment.GetEnvironmentVariable", NULL};

// ==================== NEW LANGUAGE MODULE TYPES ====================
static const char *solidity_module_types[] = {"source_file", NULL};
static const char *typst_module_types[] = {"source_file", NULL};
static const char *gdscript_module_types[] = {"source_file", NULL};
static const char *gleam_module_types[] = {"source_file", NULL};
static const char *powershell_module_types[] = {"program", NULL};
static const char *pascal_module_types[] = {"source_file", NULL};
static const char *d_module_types[] = {"source_file", NULL};
static const char *nim_module_types[] = {"source_file", NULL};
static const char *scheme_module_types[] = {"program", NULL};
static const char *fennel_module_types[] = {"program", NULL};
static const char *fish_module_types[] = {"program", NULL};
static const char *awk_module_types[] = {"program", NULL};
static const char *zsh_module_types[] = {"program", NULL};
static const char *tcl_module_types[] = {"source_file", NULL};
static const char *ada_module_types[] = {"compilation", NULL};
static const char *agda_module_types[] = {"source_file", NULL};
static const char *racket_module_types[] = {"program", NULL};
static const char *odin_module_types[] = {"source_file", NULL};
static const char *rescript_module_types[] = {"source_file", NULL};
static const char *purescript_module_types[] = {"module", NULL};
static const char *nickel_module_types[] = {"source_file", NULL};
static const char *crystal_module_types[] = {"program", NULL};
static const char *teal_module_types[] = {"program", NULL};
static const char *hare_module_types[] = {"source_file", NULL};
static const char *pony_module_types[] = {"source_file", NULL};
static const char *luau_module_types[] = {"program", NULL};
static const char *janet_module_types[] = {"source", NULL};
static const char *sway_module_types[] = {"source_file", NULL};
static const char *nasm_module_types[] = {"source_file", NULL};
static const char *assembly_module_types[] = {"program", NULL};
static const char *astro_module_types[] = {"document", NULL};
static const char *blade_module_types[] = {"document", NULL};
static const char *just_module_types[] = {"source_file", NULL};
static const char *gotemplate_module_types[] = {"template", NULL};
static const char *templ_module_types[] = {"source_file", NULL};
static const char *liquid_module_types[] = {"template", NULL};
static const char *jinja2_module_types[] = {"source_file", NULL};
static const char *prisma_module_types[] = {"program", NULL};
static const char *hyprlang_module_types[] = {"source_file", NULL};
static const char *dotenv_module_types[] = {"source_file", NULL};
static const char *diff_module_types[] = {"source", NULL};
static const char *wgsl_module_types[] = {"translation_unit", NULL};
static const char *kdl_module_types[] = {"document", NULL};
static const char *json5_module_types[] = {"document", NULL};
static const char *jsonnet_module_types[] = {"document", NULL};
static const char *ron_module_types[] = {"source_file", NULL};
static const char *thrift_module_types[] = {"document", NULL};
static const char *capnp_module_types[] = {"source", NULL};
static const char *properties_module_types[] = {"source_file", NULL};
static const char *sshconfig_module_types[] = {"source_file", NULL};
static const char *bibtex_module_types[] = {"document", NULL};
static const char *starlark_module_types[] = {"module", NULL};
static const char *bicep_module_types[] = {"program", NULL};
static const char *csv_module_types[] = {"document", NULL};
static const char *requirements_module_types[] = {"file", NULL};
static const char *hlsl_module_types[] = {"translation_unit", NULL};
static const char *vhdl_module_types[] = {"design_file", NULL};
static const char *systemverilog_module_types[] = {"source_file", NULL};
static const char *devicetree_module_types[] = {"document", NULL};
static const char *linkerscript_module_types[] = {"source_file", NULL};
static const char *gn_module_types[] = {"source_file", NULL};
static const char *kconfig_module_types[] = {"source", NULL};
static const char *bitbake_module_types[] = {"source_file", NULL};
static const char *smali_module_types[] = {"source_file", NULL};
static const char *tablegen_module_types[] = {"source_file", NULL};
static const char *ispc_module_types[] = {"translation_unit", NULL};
static const char *cairo_module_types[] = {"source_file", NULL};
static const char *move_module_types[] = {"source_file", NULL};
static const char *squirrel_module_types[] = {"source_file", NULL};
static const char *func_module_types[] = {"source_file", NULL};
static const char *regex_module_types[] = {"pattern", NULL};
static const char *jsdoc_module_types[] = {"document", NULL};
static const char *rst_module_types[] = {"document", NULL};
static const char *beancount_module_types[] = {"file", NULL};
static const char *mermaid_module_types[] = {"source_file", NULL};
static const char *puppet_module_types[] = {"source_file", NULL};
static const char *po_module_types[] = {"source_file", NULL};
static const char *gitattributes_module_types[] = {"source", NULL};
static const char *gitignore_module_types[] = {"document", NULL};
static const char *slang_module_types[] = {"source_file", NULL};
static const char *llvm_module_types[] = {"source_file", NULL};
static const char *smithy_module_types[] = {"source_file", NULL};
static const char *wit_module_types[] = {"source_file", NULL};
static const char *tlaplus_module_types[] = {"source_file", NULL};
static const char *pkl_module_types[] = {"module", NULL};
static const char *gomod_module_types[] = {"source_file", NULL};
static const char *apex_module_types[] = {"parser_output", NULL};
static const char *soql_module_types[] = {"source_file", NULL};
static const char *sosl_module_types[] = {"source_file", NULL};

// ==================== SPEC TABLE ====================

static const CBMLangSpec lang_specs[CBM_LANG_COUNT] = {
    // CBM_LANG_GO
    [CBM_LANG_GO] = {CBM_LANG_GO, go_func_types, go_class_types, go_field_types, go_module_types,
                     go_call_types, go_import_types, go_import_types, go_branch_types, go_var_types,
                     go_assign_types, empty_types, NULL, empty_types, go_env_funcs, NULL,
                     tree_sitter_go},

    // CBM_LANG_PYTHON
    [CBM_LANG_PYTHON] = {CBM_LANG_PYTHON, py_func_types, py_class_types, empty_types,
                         py_module_types, py_call_types, py_import_types, py_import_from_types,
                         py_branch_types, py_var_types, py_var_types, py_throw_types, NULL,
                         py_decorator_types, py_env_funcs, py_env_members, tree_sitter_python},

    // CBM_LANG_JAVASCRIPT
    [CBM_LANG_JAVASCRIPT] =
        {CBM_LANG_JAVASCRIPT, js_func_types, js_class_types, empty_types, js_module_types,
         js_call_types, js_import_types, js_import_types, js_branch_types, js_var_types,
         (const char *[]){"assignment_expression", "augmented_assignment_expression", NULL},
         js_throw_types, NULL, empty_types, NULL, js_env_members, tree_sitter_javascript},

    // CBM_LANG_TYPESCRIPT
    [CBM_LANG_TYPESCRIPT] =
        {CBM_LANG_TYPESCRIPT, ts_func_types, ts_class_types, empty_types, js_module_types,
         js_call_types, js_import_types, js_import_types, js_branch_types, js_var_types,
         (const char *[]){"assignment_expression", "augmented_assignment_expression", NULL},
         js_throw_types, NULL, ts_decorator_types, NULL, ts_env_members, tree_sitter_typescript},

    // CBM_LANG_TSX
    [CBM_LANG_TSX] =
        {CBM_LANG_TSX, ts_func_types, ts_class_types, empty_types, js_module_types, js_call_types,
         js_import_types, js_import_types, js_branch_types, js_var_types,
         (const char *[]){"assignment_expression", "augmented_assignment_expression", NULL},
         js_throw_types, NULL, ts_decorator_types, NULL, ts_env_members, tree_sitter_tsx},

    // CBM_LANG_RUST
    [CBM_LANG_RUST] = {CBM_LANG_RUST, rust_func_types, rust_class_types, rust_field_types,
                       rust_module_types, rust_call_types, rust_import_types,
                       rust_import_from_types, rust_branch_types, rust_var_types, rust_assign_types,
                       empty_types, NULL, rust_decorator_types, rust_env_funcs, NULL,
                       tree_sitter_rust},

    // CBM_LANG_JAVA
    [CBM_LANG_JAVA] = {CBM_LANG_JAVA, java_func_types, java_class_types, java_field_types,
                       java_module_types, java_call_types, java_import_types, java_import_types,
                       java_branch_types, java_var_types, java_assign_types, java_throw_types,
                       "throws", java_decorator_types, java_env_funcs, NULL, tree_sitter_java},

    // CBM_LANG_CPP
    [CBM_LANG_CPP] = {CBM_LANG_CPP, cpp_func_types, cpp_class_types, cpp_field_types,
                      cpp_module_types, cpp_call_types, cpp_import_types, cpp_import_types,
                      cpp_branch_types, cpp_var_types, cpp_assign_types, cpp_throw_types, NULL,
                      empty_types, cpp_env_funcs, NULL, tree_sitter_cpp},

    // CBM_LANG_CSHARP
    [CBM_LANG_CSHARP] = {CBM_LANG_CSHARP, cs_func_types, cs_class_types, empty_types,
                         cs_module_types, cs_call_types, cs_import_types, cs_import_types,
                         cs_branch_types, cs_var_types, cs_assign_types, cs_throw_types, NULL,
                         cs_decorator_types, cs_env_funcs, NULL, tree_sitter_c_sharp},

    // CBM_LANG_PHP
    [CBM_LANG_PHP] = {CBM_LANG_PHP, php_func_types, php_class_types, empty_types, php_module_types,
                      php_call_types, empty_types, empty_types, php_branch_types, php_var_types,
                      php_assign_types, php_throw_types, NULL, php_decorator_types, php_env_funcs,
                      NULL, tree_sitter_php_only},

    // CBM_LANG_LUA
    [CBM_LANG_LUA] = {CBM_LANG_LUA, lua_func_types, empty_types, empty_types, lua_module_types,
                      lua_call_types, lua_import_types, empty_types, lua_branch_types,
                      lua_var_types, lua_assign_types, empty_types, NULL, empty_types,
                      lua_env_funcs, NULL, tree_sitter_lua},

    // CBM_LANG_SCALA
    [CBM_LANG_SCALA] = {CBM_LANG_SCALA, scala_func_types, scala_class_types, empty_types,
                        scala_module_types, scala_call_types, scala_import_types,
                        scala_import_types, scala_branch_types, scala_var_types, scala_assign_types,
                        scala_throw_types, NULL, empty_types, scala_env_funcs, NULL,
                        tree_sitter_scala},

    // CBM_LANG_KOTLIN
    [CBM_LANG_KOTLIN] = {CBM_LANG_KOTLIN, kotlin_func_types, kotlin_class_types, empty_types,
                         kotlin_module_types, kotlin_call_types, kotlin_import_types,
                         kotlin_import_types, kotlin_branch_types, kotlin_var_types,
                         kotlin_assign_types, kotlin_throw_types, NULL, kotlin_decorator_types,
                         kotlin_env_funcs, NULL, tree_sitter_kotlin},

    // CBM_LANG_RUBY
    [CBM_LANG_RUBY] = {CBM_LANG_RUBY, ruby_func_types, ruby_class_types, empty_types,
                       ruby_module_types, ruby_call_types, ruby_import_types, empty_types,
                       ruby_branch_types, ruby_var_types, ruby_assign_types, empty_types, NULL,
                       empty_types, NULL, ruby_env_members, tree_sitter_ruby},

    // CBM_LANG_C
    [CBM_LANG_C] = {CBM_LANG_C, c_func_types, c_class_types, c_field_types, c_module_types,
                    c_call_types, c_import_types, empty_types, c_branch_types, c_var_types,
                    c_assign_types, empty_types, NULL, empty_types, c_env_funcs, NULL,
                    tree_sitter_c},

    // CBM_LANG_BASH
    [CBM_LANG_BASH] = {CBM_LANG_BASH, bash_func_types, empty_types, empty_types, bash_module_types,
                       bash_call_types, bash_import_types, empty_types, bash_branch_types,
                       bash_var_types, bash_var_types, empty_types, NULL, empty_types, NULL, NULL,
                       tree_sitter_bash},

    // CBM_LANG_ZIG
    [CBM_LANG_ZIG] = {CBM_LANG_ZIG, zig_func_types, zig_class_types, zig_field_types,
                      zig_module_types, zig_call_types, zig_import_types, empty_types,
                      zig_branch_types, zig_var_types, zig_assign_types, empty_types, NULL,
                      empty_types, zig_env_funcs, NULL, tree_sitter_zig},

    // CBM_LANG_ELIXIR
    [CBM_LANG_ELIXIR] = {CBM_LANG_ELIXIR, elixir_func_types, empty_types, empty_types,
                         elixir_module_types, elixir_call_types, elixir_import_types, empty_types,
                         elixir_branch_types, elixir_var_types, elixir_var_types, empty_types, NULL,
                         empty_types, elixir_env_funcs, NULL, tree_sitter_elixir},

    // CBM_LANG_HASKELL
    [CBM_LANG_HASKELL] = {CBM_LANG_HASKELL, haskell_func_types, haskell_class_types, empty_types,
                          haskell_module_types, haskell_call_types, haskell_import_types,
                          empty_types, haskell_branch_types, haskell_var_types, haskell_var_types,
                          empty_types, NULL, empty_types, haskell_env_funcs, NULL,
                          tree_sitter_haskell},

    // CBM_LANG_OCAML
    [CBM_LANG_OCAML] = {CBM_LANG_OCAML, ocaml_func_types, ocaml_class_types, empty_types,
                        ocaml_module_types, ocaml_call_types, ocaml_import_types, empty_types,
                        ocaml_branch_types, ocaml_var_types, ocaml_var_types, empty_types, NULL,
                        empty_types, ocaml_env_funcs, NULL, tree_sitter_ocaml},

    // CBM_LANG_OBJC
    [CBM_LANG_OBJC] = {CBM_LANG_OBJC, objc_func_types, objc_class_types, objc_field_types,
                       objc_module_types, objc_call_types, objc_import_types, empty_types,
                       objc_branch_types, objc_var_types, objc_assign_types, empty_types, NULL,
                       empty_types, NULL, NULL, tree_sitter_objc},

    // CBM_LANG_SWIFT
    [CBM_LANG_SWIFT] = {CBM_LANG_SWIFT, swift_func_types, swift_class_types, swift_field_types,
                        swift_module_types, swift_call_types, swift_import_types, empty_types,
                        swift_branch_types, swift_var_types, swift_assign_types, swift_throw_types,
                        NULL, swift_decorator_types, NULL, NULL, tree_sitter_swift},

    // CBM_LANG_DART
    [CBM_LANG_DART] = {CBM_LANG_DART, dart_func_types, dart_class_types, dart_field_types,
                       dart_module_types, dart_call_types, dart_import_types, empty_types,
                       dart_branch_types, dart_var_types, dart_assign_types, dart_throw_types, NULL,
                       dart_decorator_types, NULL, NULL, tree_sitter_dart},

    // CBM_LANG_PERL
    [CBM_LANG_PERL] = {CBM_LANG_PERL, perl_func_types, empty_types, empty_types, perl_module_types,
                       perl_call_types, perl_import_types, empty_types, perl_branch_types,
                       perl_var_types, perl_assign_types, empty_types, NULL, empty_types,
                       perl_env_funcs, NULL, tree_sitter_perl},

    // CBM_LANG_GROOVY
    [CBM_LANG_GROOVY] = {CBM_LANG_GROOVY, groovy_func_types, groovy_class_types, empty_types,
                         groovy_module_types, groovy_call_types, groovy_import_types, empty_types,
                         groovy_branch_types, groovy_var_types, groovy_assign_types,
                         groovy_throw_types, NULL, groovy_decorator_types, NULL, NULL,
                         tree_sitter_groovy},

    // CBM_LANG_ERLANG
    [CBM_LANG_ERLANG] = {CBM_LANG_ERLANG, erlang_func_types, empty_types, empty_types,
                         erlang_module_types, erlang_call_types, erlang_import_types, empty_types,
                         erlang_branch_types, erlang_var_types, erlang_assign_types,
                         erlang_throw_types, NULL, empty_types, NULL, NULL, tree_sitter_erlang},

    // CBM_LANG_R
    [CBM_LANG_R] = {CBM_LANG_R, r_func_types, empty_types, empty_types, r_module_types,
                    r_call_types, r_import_types, empty_types, r_branch_types, r_var_types,
                    r_var_types, empty_types, NULL, empty_types, r_env_funcs, NULL, tree_sitter_r},

    // CBM_LANG_HTML
    [CBM_LANG_HTML] = {CBM_LANG_HTML, empty_types, empty_types, empty_types, html_module_types,
                       empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                       empty_types, NULL, empty_types, NULL, NULL, tree_sitter_html},

    // CBM_LANG_CSS
    [CBM_LANG_CSS] = {CBM_LANG_CSS, empty_types, empty_types, empty_types, css_module_types,
                      empty_types, css_import_types, empty_types, empty_types, empty_types,
                      empty_types, empty_types, NULL, empty_types, NULL, NULL, tree_sitter_css},

    // CBM_LANG_SCSS
    [CBM_LANG_SCSS] = {CBM_LANG_SCSS, scss_func_types, empty_types, empty_types, scss_module_types,
                       scss_call_types, scss_import_types, empty_types, scss_branch_types,
                       scss_var_types, empty_types, empty_types, NULL, empty_types, NULL, NULL,
                       tree_sitter_scss},

    // CBM_LANG_YAML
    [CBM_LANG_YAML] = {CBM_LANG_YAML, empty_types, empty_types, empty_types, yaml_module_types,
                       empty_types, empty_types, empty_types, empty_types, yaml_var_types,
                       empty_types, empty_types, NULL, empty_types, NULL, NULL, tree_sitter_yaml},

    // CBM_LANG_TOML
    [CBM_LANG_TOML] = {CBM_LANG_TOML, empty_types, toml_class_types, empty_types, toml_module_types,
                       empty_types, empty_types, empty_types, empty_types, toml_var_types,
                       empty_types, empty_types, NULL, empty_types, NULL, NULL, tree_sitter_toml},

    // CBM_LANG_HCL
    [CBM_LANG_HCL] = {CBM_LANG_HCL, empty_types, hcl_class_types, empty_types, hcl_module_types,
                      hcl_call_types, empty_types, empty_types, empty_types, hcl_var_types,
                      empty_types, empty_types, NULL, empty_types, NULL, NULL, tree_sitter_hcl},

    // CBM_LANG_SQL
    [CBM_LANG_SQL] = {CBM_LANG_SQL, sql_func_types, empty_types, sql_field_types, sql_module_types,
                      sql_call_types, empty_types, empty_types, sql_branch_types, sql_var_types,
                      empty_types, empty_types, NULL, empty_types, NULL, NULL, tree_sitter_sql},

    // CBM_LANG_DOCKERFILE
    [CBM_LANG_DOCKERFILE] = {CBM_LANG_DOCKERFILE, empty_types, empty_types, empty_types,
                             dockerfile_module_types, empty_types, empty_types, empty_types,
                             empty_types, dockerfile_var_types, empty_types, empty_types, NULL,
                             empty_types, NULL, NULL, tree_sitter_dockerfile},

    // CBM_LANG_CLOJURE
    [CBM_LANG_CLOJURE] = {CBM_LANG_CLOJURE, empty_types, empty_types, empty_types,
                          clojure_module_types, clojure_call_types, empty_types, empty_types,
                          empty_types, empty_types, empty_types, empty_types, NULL, empty_types,
                          NULL, NULL, tree_sitter_clojure},

    // CBM_LANG_FSHARP
    [CBM_LANG_FSHARP] = {CBM_LANG_FSHARP, fsharp_func_types, fsharp_class_types, empty_types,
                         fsharp_module_types, fsharp_call_types, fsharp_import_types, empty_types,
                         fsharp_branch_types, fsharp_var_types, fsharp_var_types, empty_types, NULL,
                         empty_types, fsharp_env_funcs, NULL, tree_sitter_fsharp},

    // CBM_LANG_JULIA
    [CBM_LANG_JULIA] = {CBM_LANG_JULIA, julia_func_types, julia_class_types, empty_types,
                        julia_module_types, julia_call_types, julia_import_types, empty_types,
                        julia_branch_types, julia_var_types, julia_assign_types, julia_throw_types,
                        NULL, empty_types, julia_env_funcs, NULL, tree_sitter_julia},

    // CBM_LANG_VIMSCRIPT
    [CBM_LANG_VIMSCRIPT] = {CBM_LANG_VIMSCRIPT, vim_func_types, empty_types, empty_types,
                            vim_module_types, vim_call_types, empty_types, empty_types,
                            vim_branch_types, vim_var_types, vim_var_types, empty_types, NULL,
                            empty_types, NULL, NULL, tree_sitter_vim},

    // CBM_LANG_NIX
    [CBM_LANG_NIX] = {CBM_LANG_NIX, nix_func_types, empty_types, empty_types, nix_module_types,
                      nix_call_types, empty_types, empty_types, nix_branch_types, nix_var_types,
                      nix_var_types, empty_types, NULL, empty_types, nix_env_funcs, NULL,
                      tree_sitter_nix},

    // CBM_LANG_COMMONLISP
    [CBM_LANG_COMMONLISP] = {CBM_LANG_COMMONLISP, commonlisp_func_types, empty_types, empty_types,
                             commonlisp_module_types, commonlisp_call_types, empty_types,
                             empty_types, empty_types, empty_types, empty_types, empty_types, NULL,
                             empty_types, NULL, NULL, tree_sitter_commonlisp},

    // CBM_LANG_ELM
    [CBM_LANG_ELM] = {CBM_LANG_ELM, elm_func_types, elm_class_types, empty_types, elm_module_types,
                      elm_call_types, elm_import_types, empty_types, elm_branch_types, empty_types,
                      empty_types, empty_types, NULL, empty_types, NULL, NULL, tree_sitter_elm},

    // CBM_LANG_FORTRAN
    [CBM_LANG_FORTRAN] = {CBM_LANG_FORTRAN, fortran_func_types, fortran_class_types, empty_types,
                          fortran_module_types, fortran_call_types, fortran_import_types,
                          empty_types, fortran_branch_types, fortran_var_types,
                          fortran_assign_types, empty_types, NULL, empty_types, fortran_env_funcs,
                          NULL, tree_sitter_fortran},

    // CBM_LANG_CUDA (reuses C++ node types)
    [CBM_LANG_CUDA] = {CBM_LANG_CUDA, cpp_func_types, cpp_class_types, cpp_field_types,
                       cpp_module_types, cpp_call_types, cpp_import_types, cpp_import_types,
                       cpp_branch_types, cpp_var_types, cpp_assign_types, cpp_throw_types, NULL,
                       empty_types, cpp_env_funcs, NULL, tree_sitter_cuda},

    // CBM_LANG_COBOL
    [CBM_LANG_COBOL] = {CBM_LANG_COBOL, cobol_func_types, empty_types, empty_types,
                        cobol_module_types, cobol_call_types, empty_types, empty_types,
                        cobol_branch_types, cobol_var_types, empty_types, empty_types, NULL,
                        empty_types, NULL, NULL, tree_sitter_COBOL},

    // CBM_LANG_VERILOG
    [CBM_LANG_VERILOG] = {CBM_LANG_VERILOG, verilog_func_types, verilog_class_types, empty_types,
                          verilog_module_types, verilog_call_types, empty_types, empty_types,
                          verilog_branch_types, verilog_var_types, verilog_assign_types,
                          empty_types, NULL, empty_types, NULL, NULL, tree_sitter_verilog},

    // CBM_LANG_EMACSLISP
    [CBM_LANG_EMACSLISP] = {CBM_LANG_EMACSLISP, elisp_func_types, empty_types, empty_types,
                            elisp_module_types, elisp_call_types, empty_types, empty_types,
                            empty_types, empty_types, empty_types, empty_types, NULL, empty_types,
                            NULL, NULL, tree_sitter_elisp},

    // CBM_LANG_JSON
    [CBM_LANG_JSON] = {CBM_LANG_JSON, empty_types, empty_types, empty_types, json_module_types,
                       empty_types, empty_types, empty_types, empty_types, json_var_types,
                       empty_types, empty_types, NULL, empty_types, NULL, NULL, tree_sitter_json},

    // CBM_LANG_XML
    [CBM_LANG_XML] = {CBM_LANG_XML, empty_types, xml_class_types, empty_types, xml_module_types,
                      empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                      empty_types, NULL, empty_types, NULL, NULL, tree_sitter_xml},

    // CBM_LANG_MARKDOWN
    [CBM_LANG_MARKDOWN] = {CBM_LANG_MARKDOWN, empty_types, markdown_class_types, empty_types,
                           markdown_module_types, empty_types, empty_types, empty_types,
                           empty_types, empty_types, empty_types, empty_types, NULL, empty_types,
                           NULL, NULL, tree_sitter_markdown},

    // CBM_LANG_MAKEFILE
    [CBM_LANG_MAKEFILE] = {CBM_LANG_MAKEFILE, makefile_func_types, empty_types, empty_types,
                           makefile_module_types, makefile_call_types, makefile_import_types,
                           empty_types, empty_types, makefile_var_types, empty_types, empty_types,
                           NULL, empty_types, NULL, NULL, tree_sitter_make},

    // CBM_LANG_CMAKE
    [CBM_LANG_CMAKE] = {CBM_LANG_CMAKE, empty_types, empty_types, empty_types, cmake_module_types,
                        cmake_call_types, empty_types, empty_types, empty_types, empty_types,
                        empty_types, empty_types, NULL, empty_types, NULL, NULL, tree_sitter_cmake},

    // CBM_LANG_PROTOBUF
    [CBM_LANG_PROTOBUF] = {CBM_LANG_PROTOBUF, empty_types, protobuf_class_types,
                           protobuf_field_types, protobuf_module_types, empty_types,
                           protobuf_import_types, empty_types, empty_types, empty_types,
                           empty_types, empty_types, NULL, empty_types, NULL, NULL,
                           tree_sitter_proto},

    // CBM_LANG_GRAPHQL
    [CBM_LANG_GRAPHQL] = {CBM_LANG_GRAPHQL, empty_types, graphql_class_types, graphql_field_types,
                          graphql_module_types, empty_types, empty_types, empty_types, empty_types,
                          empty_types, empty_types, empty_types, NULL, empty_types, NULL, NULL,
                          tree_sitter_graphql},

    // CBM_LANG_VUE
    [CBM_LANG_VUE] = {CBM_LANG_VUE, empty_types, empty_types, empty_types, vue_module_types,
                      empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                      empty_types, NULL, empty_types, NULL, NULL, tree_sitter_vue},

    // CBM_LANG_SVELTE
    [CBM_LANG_SVELTE] = {CBM_LANG_SVELTE, empty_types, empty_types, empty_types,
                         svelte_module_types, empty_types, empty_types, empty_types,
                         svelte_branch_types, empty_types, empty_types, empty_types, NULL,
                         empty_types, NULL, NULL, tree_sitter_svelte},

    // CBM_LANG_MESON
    [CBM_LANG_MESON] = {CBM_LANG_MESON, meson_func_types, empty_types, empty_types,
                        meson_module_types, meson_call_types, empty_types, empty_types,
                        meson_branch_types, meson_var_types, meson_var_types, empty_types, NULL,
                        empty_types, NULL, NULL, tree_sitter_meson},

    // CBM_LANG_GLSL (reuses C node types)
    [CBM_LANG_GLSL] = {CBM_LANG_GLSL, c_func_types, c_class_types, c_field_types, c_module_types,
                       c_call_types, c_import_types, empty_types, c_branch_types, c_var_types,
                       c_assign_types, empty_types, NULL, empty_types, NULL, NULL,
                       tree_sitter_glsl},

    // CBM_LANG_INI
    [CBM_LANG_INI] = {CBM_LANG_INI, empty_types, ini_class_types, empty_types, ini_module_types,
                      empty_types, empty_types, empty_types, empty_types, ini_var_types,
                      empty_types, empty_types, NULL, empty_types, NULL, NULL, tree_sitter_ini},

    // CBM_LANG_MATLAB
    [CBM_LANG_MATLAB] = {CBM_LANG_MATLAB, matlab_func_types, matlab_class_types, empty_types,
                         matlab_module_types, matlab_call_types, empty_types, empty_types,
                         matlab_branch_types, matlab_var_types, matlab_var_types, empty_types, NULL,
                         empty_types, NULL, NULL, tree_sitter_matlab},

    // CBM_LANG_LEAN
    [CBM_LANG_LEAN] = {CBM_LANG_LEAN, lean_func_types, lean_class_types, empty_types,
                       lean_module_types, lean_call_types, lean_import_types, empty_types,
                       lean_branch_types, empty_types, empty_types, empty_types, NULL, empty_types,
                       NULL, NULL, tree_sitter_lean},

    // CBM_LANG_FORM
    [CBM_LANG_FORM] = {CBM_LANG_FORM, form_func_types, empty_types, empty_types, form_module_types,
                       form_call_types, form_import_types, empty_types, form_branch_types,
                       form_var_types, form_assign_types, empty_types, NULL, empty_types, NULL,
                       NULL, tree_sitter_form},

    // CBM_LANG_MAGMA
    [CBM_LANG_MAGMA] = {CBM_LANG_MAGMA, magma_func_types, empty_types, empty_types,
                        magma_module_types, magma_call_types, magma_import_types, empty_types,
                        magma_branch_types, magma_var_types, magma_var_types, empty_types, NULL,
                        empty_types, NULL, NULL, tree_sitter_magma},

    // CBM_LANG_WOLFRAM
    [CBM_LANG_WOLFRAM] = {CBM_LANG_WOLFRAM, wolfram_func_types, empty_types, empty_types,
                          wolfram_module_types, wolfram_call_types, wolfram_import_types,
                          empty_types, empty_types, empty_types, empty_types, empty_types, NULL,
                          empty_types, NULL, NULL, tree_sitter_wolfram},

    // CBM_LANG_SOLIDITY
    [CBM_LANG_SOLIDITY] = {CBM_LANG_SOLIDITY, empty_types, empty_types, empty_types,
                           solidity_module_types, empty_types, empty_types, empty_types,
                           empty_types, empty_types, empty_types, empty_types, NULL, empty_types,
                           NULL, NULL, tree_sitter_solidity},

    // CBM_LANG_TYPST
    [CBM_LANG_TYPST] = {CBM_LANG_TYPST, empty_types, empty_types, empty_types, typst_module_types,
                        empty_types, empty_types, empty_types, empty_types, empty_types,
                        empty_types, empty_types, NULL, empty_types, NULL, NULL, tree_sitter_typst},

    // CBM_LANG_GDSCRIPT
    [CBM_LANG_GDSCRIPT] = {CBM_LANG_GDSCRIPT, empty_types, empty_types, empty_types,
                           gdscript_module_types, empty_types, empty_types, empty_types,
                           empty_types, empty_types, empty_types, empty_types, NULL, empty_types,
                           NULL, NULL, tree_sitter_gdscript},

    // CBM_LANG_GLEAM
    [CBM_LANG_GLEAM] = {CBM_LANG_GLEAM, empty_types, empty_types, empty_types, gleam_module_types,
                        empty_types, empty_types, empty_types, empty_types, empty_types,
                        empty_types, empty_types, NULL, empty_types, NULL, NULL, tree_sitter_gleam},

    // CBM_LANG_POWERSHELL
    [CBM_LANG_POWERSHELL] = {CBM_LANG_POWERSHELL, empty_types, empty_types, empty_types,
                             powershell_module_types, empty_types, empty_types, empty_types,
                             empty_types, empty_types, empty_types, empty_types, NULL, empty_types,
                             NULL, NULL, tree_sitter_powershell},

    // CBM_LANG_PASCAL
    [CBM_LANG_PASCAL] = {CBM_LANG_PASCAL, empty_types, empty_types, empty_types,
                         pascal_module_types, empty_types, empty_types, empty_types, empty_types,
                         empty_types, empty_types, empty_types, NULL, empty_types, NULL, NULL,
                         tree_sitter_pascal},

    // CBM_LANG_DLANG
    [CBM_LANG_DLANG] = {CBM_LANG_DLANG, empty_types, empty_types, empty_types, d_module_types,
                        empty_types, empty_types, empty_types, empty_types, empty_types,
                        empty_types, empty_types, NULL, empty_types, NULL, NULL, tree_sitter_d},

    // CBM_LANG_NIM
    [CBM_LANG_NIM] = {CBM_LANG_NIM, empty_types, empty_types, empty_types, nim_module_types,
                      empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                      empty_types, NULL, empty_types, NULL, NULL, tree_sitter_nim},

    // CBM_LANG_SCHEME
    [CBM_LANG_SCHEME] = {CBM_LANG_SCHEME, empty_types, empty_types, empty_types,
                         scheme_module_types, empty_types, empty_types, empty_types, empty_types,
                         empty_types, empty_types, empty_types, NULL, empty_types, NULL, NULL,
                         tree_sitter_scheme},

    // CBM_LANG_FENNEL
    [CBM_LANG_FENNEL] = {CBM_LANG_FENNEL, empty_types, empty_types, empty_types,
                         fennel_module_types, empty_types, empty_types, empty_types, empty_types,
                         empty_types, empty_types, empty_types, NULL, empty_types, NULL, NULL,
                         tree_sitter_fennel},

    // CBM_LANG_FISH
    [CBM_LANG_FISH] = {CBM_LANG_FISH, empty_types, empty_types, empty_types, fish_module_types,
                       empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                       empty_types, NULL, empty_types, NULL, NULL, tree_sitter_fish},

    // CBM_LANG_AWK
    [CBM_LANG_AWK] = {CBM_LANG_AWK, empty_types, empty_types, empty_types, awk_module_types,
                      empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                      empty_types, NULL, empty_types, NULL, NULL, tree_sitter_awk},

    // CBM_LANG_ZSH
    [CBM_LANG_ZSH] = {CBM_LANG_ZSH, empty_types, empty_types, empty_types, zsh_module_types,
                      empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                      empty_types, NULL, empty_types, NULL, NULL, tree_sitter_zsh},

    // CBM_LANG_TCL
    [CBM_LANG_TCL] = {CBM_LANG_TCL, empty_types, empty_types, empty_types, tcl_module_types,
                      empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                      empty_types, NULL, empty_types, NULL, NULL, tree_sitter_tcl},

    // CBM_LANG_ADA
    [CBM_LANG_ADA] = {CBM_LANG_ADA, empty_types, empty_types, empty_types, ada_module_types,
                      empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                      empty_types, NULL, empty_types, NULL, NULL, tree_sitter_ada},

    // CBM_LANG_AGDA
    [CBM_LANG_AGDA] = {CBM_LANG_AGDA, empty_types, empty_types, empty_types, agda_module_types,
                       empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                       empty_types, NULL, empty_types, NULL, NULL, tree_sitter_agda},

    // CBM_LANG_RACKET
    [CBM_LANG_RACKET] = {CBM_LANG_RACKET, empty_types, empty_types, empty_types,
                         racket_module_types, empty_types, empty_types, empty_types, empty_types,
                         empty_types, empty_types, empty_types, NULL, empty_types, NULL, NULL,
                         tree_sitter_racket},

    // CBM_LANG_ODIN
    [CBM_LANG_ODIN] = {CBM_LANG_ODIN, empty_types, empty_types, empty_types, odin_module_types,
                       empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                       empty_types, NULL, empty_types, NULL, NULL, tree_sitter_odin},

    // CBM_LANG_RESCRIPT
    [CBM_LANG_RESCRIPT] = {CBM_LANG_RESCRIPT, empty_types, empty_types, empty_types,
                           rescript_module_types, empty_types, empty_types, empty_types,
                           empty_types, empty_types, empty_types, empty_types, NULL, empty_types,
                           NULL, NULL, tree_sitter_rescript},

    // CBM_LANG_PURESCRIPT
    [CBM_LANG_PURESCRIPT] = {CBM_LANG_PURESCRIPT, empty_types, empty_types, empty_types,
                             purescript_module_types, empty_types, empty_types, empty_types,
                             empty_types, empty_types, empty_types, empty_types, NULL, empty_types,
                             NULL, NULL, tree_sitter_purescript},

    // CBM_LANG_NICKEL
    [CBM_LANG_NICKEL] = {CBM_LANG_NICKEL, empty_types, empty_types, empty_types,
                         nickel_module_types, empty_types, empty_types, empty_types, empty_types,
                         empty_types, empty_types, empty_types, NULL, empty_types, NULL, NULL,
                         tree_sitter_nickel},

    // CBM_LANG_CRYSTAL
    [CBM_LANG_CRYSTAL] = {CBM_LANG_CRYSTAL, empty_types, empty_types, empty_types,
                          crystal_module_types, empty_types, empty_types, empty_types, empty_types,
                          empty_types, empty_types, empty_types, NULL, empty_types, NULL, NULL,
                          tree_sitter_crystal},

    // CBM_LANG_TEAL
    [CBM_LANG_TEAL] = {CBM_LANG_TEAL, empty_types, empty_types, empty_types, teal_module_types,
                       empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                       empty_types, NULL, empty_types, NULL, NULL, tree_sitter_teal},

    // CBM_LANG_HARE
    [CBM_LANG_HARE] = {CBM_LANG_HARE, empty_types, empty_types, empty_types, hare_module_types,
                       empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                       empty_types, NULL, empty_types, NULL, NULL, tree_sitter_hare},

    // CBM_LANG_PONY
    [CBM_LANG_PONY] = {CBM_LANG_PONY, empty_types, empty_types, empty_types, pony_module_types,
                       empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                       empty_types, NULL, empty_types, NULL, NULL, tree_sitter_pony},

    // CBM_LANG_LUAU
    [CBM_LANG_LUAU] = {CBM_LANG_LUAU, empty_types, empty_types, empty_types, luau_module_types,
                       empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                       empty_types, NULL, empty_types, NULL, NULL, tree_sitter_luau},

    // CBM_LANG_JANET
    [CBM_LANG_JANET] = {CBM_LANG_JANET, empty_types, empty_types, empty_types, janet_module_types,
                        empty_types, empty_types, empty_types, empty_types, empty_types,
                        empty_types, empty_types, NULL, empty_types, NULL, NULL,
                        tree_sitter_janet_simple},

    // CBM_LANG_SWAY
    [CBM_LANG_SWAY] = {CBM_LANG_SWAY, empty_types, empty_types, empty_types, sway_module_types,
                       empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                       empty_types, NULL, empty_types, NULL, NULL, tree_sitter_sway},

    // CBM_LANG_NASM
    [CBM_LANG_NASM] = {CBM_LANG_NASM, empty_types, empty_types, empty_types, nasm_module_types,
                       empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                       empty_types, NULL, empty_types, NULL, NULL, tree_sitter_nasm},

    // CBM_LANG_ASSEMBLY
    [CBM_LANG_ASSEMBLY] = {CBM_LANG_ASSEMBLY, empty_types, empty_types, empty_types,
                           assembly_module_types, empty_types, empty_types, empty_types,
                           empty_types, empty_types, empty_types, empty_types, NULL, empty_types,
                           NULL, NULL, tree_sitter_asm},

    // CBM_LANG_ASTRO
    [CBM_LANG_ASTRO] = {CBM_LANG_ASTRO, empty_types, empty_types, empty_types, astro_module_types,
                        empty_types, empty_types, empty_types, empty_types, empty_types,
                        empty_types, empty_types, NULL, empty_types, NULL, NULL, tree_sitter_astro},

    // CBM_LANG_BLADE
    [CBM_LANG_BLADE] = {CBM_LANG_BLADE, empty_types, empty_types, empty_types, blade_module_types,
                        empty_types, empty_types, empty_types, empty_types, empty_types,
                        empty_types, empty_types, NULL, empty_types, NULL, NULL, tree_sitter_blade},

    // CBM_LANG_JUST
    [CBM_LANG_JUST] = {CBM_LANG_JUST, empty_types, empty_types, empty_types, just_module_types,
                       empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                       empty_types, NULL, empty_types, NULL, NULL, tree_sitter_just},

    // CBM_LANG_GOTEMPLATE
    [CBM_LANG_GOTEMPLATE] = {CBM_LANG_GOTEMPLATE, empty_types, empty_types, empty_types,
                             gotemplate_module_types, empty_types, empty_types, empty_types,
                             empty_types, empty_types, empty_types, empty_types, NULL, empty_types,
                             NULL, NULL, tree_sitter_gotmpl},

    // CBM_LANG_TEMPL
    [CBM_LANG_TEMPL] = {CBM_LANG_TEMPL, empty_types, empty_types, empty_types, templ_module_types,
                        empty_types, empty_types, empty_types, empty_types, empty_types,
                        empty_types, empty_types, NULL, empty_types, NULL, NULL, tree_sitter_templ},

    // CBM_LANG_LIQUID
    [CBM_LANG_LIQUID] = {CBM_LANG_LIQUID, empty_types, empty_types, empty_types,
                         liquid_module_types, empty_types, empty_types, empty_types, empty_types,
                         empty_types, empty_types, empty_types, NULL, empty_types, NULL, NULL,
                         tree_sitter_liquid},

    // CBM_LANG_JINJA2
    [CBM_LANG_JINJA2] = {CBM_LANG_JINJA2, empty_types, empty_types, empty_types,
                         jinja2_module_types, empty_types, empty_types, empty_types, empty_types,
                         empty_types, empty_types, empty_types, NULL, empty_types, NULL, NULL,
                         tree_sitter_jinja2},

    // CBM_LANG_PRISMA
    [CBM_LANG_PRISMA] = {CBM_LANG_PRISMA, empty_types, empty_types, empty_types,
                         prisma_module_types, empty_types, empty_types, empty_types, empty_types,
                         empty_types, empty_types, empty_types, NULL, empty_types, NULL, NULL,
                         tree_sitter_prisma},

    // CBM_LANG_HYPRLANG
    [CBM_LANG_HYPRLANG] = {CBM_LANG_HYPRLANG, empty_types, empty_types, empty_types,
                           hyprlang_module_types, empty_types, empty_types, empty_types,
                           empty_types, empty_types, empty_types, empty_types, NULL, empty_types,
                           NULL, NULL, tree_sitter_hyprlang},

    // CBM_LANG_DOTENV
    [CBM_LANG_DOTENV] = {CBM_LANG_DOTENV, empty_types, empty_types, empty_types,
                         dotenv_module_types, empty_types, empty_types, empty_types, empty_types,
                         empty_types, empty_types, empty_types, NULL, empty_types, NULL, NULL,
                         tree_sitter_dotenv},

    // CBM_LANG_DIFF
    [CBM_LANG_DIFF] = {CBM_LANG_DIFF, empty_types, empty_types, empty_types, diff_module_types,
                       empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                       empty_types, NULL, empty_types, NULL, NULL, tree_sitter_diff},

    // CBM_LANG_WGSL
    [CBM_LANG_WGSL] = {CBM_LANG_WGSL, empty_types, empty_types, empty_types, wgsl_module_types,
                       empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                       empty_types, NULL, empty_types, NULL, NULL, tree_sitter_wgsl},

    // CBM_LANG_KDL
    [CBM_LANG_KDL] = {CBM_LANG_KDL, empty_types, empty_types, empty_types, kdl_module_types,
                      empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                      empty_types, NULL, empty_types, NULL, NULL, tree_sitter_kdl},

    // CBM_LANG_JSON5
    [CBM_LANG_JSON5] = {CBM_LANG_JSON5, empty_types, empty_types, empty_types, json5_module_types,
                        empty_types, empty_types, empty_types, empty_types, empty_types,
                        empty_types, empty_types, NULL, empty_types, NULL, NULL, tree_sitter_json5},

    // CBM_LANG_JSONNET
    [CBM_LANG_JSONNET] = {CBM_LANG_JSONNET, empty_types, empty_types, empty_types,
                          jsonnet_module_types, empty_types, empty_types, empty_types, empty_types,
                          empty_types, empty_types, empty_types, NULL, empty_types, NULL, NULL,
                          tree_sitter_jsonnet},

    // CBM_LANG_RON
    [CBM_LANG_RON] = {CBM_LANG_RON, empty_types, empty_types, empty_types, ron_module_types,
                      empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                      empty_types, NULL, empty_types, NULL, NULL, tree_sitter_ron},

    // CBM_LANG_THRIFT
    [CBM_LANG_THRIFT] = {CBM_LANG_THRIFT, empty_types, empty_types, empty_types,
                         thrift_module_types, empty_types, empty_types, empty_types, empty_types,
                         empty_types, empty_types, empty_types, NULL, empty_types, NULL, NULL,
                         tree_sitter_thrift},

    // CBM_LANG_CAPNP
    [CBM_LANG_CAPNP] = {CBM_LANG_CAPNP, empty_types, empty_types, empty_types, capnp_module_types,
                        empty_types, empty_types, empty_types, empty_types, empty_types,
                        empty_types, empty_types, NULL, empty_types, NULL, NULL, tree_sitter_capnp},

    // CBM_LANG_PROPERTIES
    [CBM_LANG_PROPERTIES] = {CBM_LANG_PROPERTIES, empty_types, empty_types, empty_types,
                             properties_module_types, empty_types, empty_types, empty_types,
                             empty_types, empty_types, empty_types, empty_types, NULL, empty_types,
                             NULL, NULL, tree_sitter_properties},

    // CBM_LANG_SSHCONFIG
    [CBM_LANG_SSHCONFIG] = {CBM_LANG_SSHCONFIG, empty_types, empty_types, empty_types,
                            sshconfig_module_types, empty_types, empty_types, empty_types,
                            empty_types, empty_types, empty_types, empty_types, NULL, empty_types,
                            NULL, NULL, tree_sitter_ssh_config},

    // CBM_LANG_BIBTEX
    [CBM_LANG_BIBTEX] = {CBM_LANG_BIBTEX, empty_types, empty_types, empty_types,
                         bibtex_module_types, empty_types, empty_types, empty_types, empty_types,
                         empty_types, empty_types, empty_types, NULL, empty_types, NULL, NULL,
                         tree_sitter_bibtex},

    // CBM_LANG_STARLARK
    [CBM_LANG_STARLARK] = {CBM_LANG_STARLARK, empty_types, empty_types, empty_types,
                           starlark_module_types, empty_types, empty_types, empty_types,
                           empty_types, empty_types, empty_types, empty_types, NULL, empty_types,
                           NULL, NULL, tree_sitter_starlark},

    // CBM_LANG_BICEP
    [CBM_LANG_BICEP] = {CBM_LANG_BICEP, empty_types, empty_types, empty_types, bicep_module_types,
                        empty_types, empty_types, empty_types, empty_types, empty_types,
                        empty_types, empty_types, NULL, empty_types, NULL, NULL, tree_sitter_bicep},

    // CBM_LANG_CSV
    [CBM_LANG_CSV] = {CBM_LANG_CSV, empty_types, empty_types, empty_types, csv_module_types,
                      empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                      empty_types, NULL, empty_types, NULL, NULL, tree_sitter_csv},

    // CBM_LANG_REQUIREMENTS
    [CBM_LANG_REQUIREMENTS] = {CBM_LANG_REQUIREMENTS, empty_types, empty_types, empty_types,
                               requirements_module_types, empty_types, empty_types, empty_types,
                               empty_types, empty_types, empty_types, empty_types, NULL,
                               empty_types, NULL, NULL, tree_sitter_requirements},

    // CBM_LANG_HLSL
    [CBM_LANG_HLSL] = {CBM_LANG_HLSL, empty_types, empty_types, empty_types, hlsl_module_types,
                       empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                       empty_types, NULL, empty_types, NULL, NULL, tree_sitter_hlsl},

    // CBM_LANG_VHDL
    [CBM_LANG_VHDL] = {CBM_LANG_VHDL, empty_types, empty_types, empty_types, vhdl_module_types,
                       empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                       empty_types, NULL, empty_types, NULL, NULL, tree_sitter_vhdl},

    // CBM_LANG_SYSTEMVERILOG
    [CBM_LANG_SYSTEMVERILOG] = {CBM_LANG_SYSTEMVERILOG, empty_types, empty_types, empty_types,
                                systemverilog_module_types, empty_types, empty_types, empty_types,
                                empty_types, empty_types, empty_types, empty_types, NULL,
                                empty_types, NULL, NULL, tree_sitter_systemverilog},

    // CBM_LANG_DEVICETREE
    [CBM_LANG_DEVICETREE] = {CBM_LANG_DEVICETREE, empty_types, empty_types, empty_types,
                             devicetree_module_types, empty_types, empty_types, empty_types,
                             empty_types, empty_types, empty_types, empty_types, NULL, empty_types,
                             NULL, NULL, tree_sitter_devicetree},

    // CBM_LANG_LINKERSCRIPT
    [CBM_LANG_LINKERSCRIPT] = {CBM_LANG_LINKERSCRIPT, empty_types, empty_types, empty_types,
                               linkerscript_module_types, empty_types, empty_types, empty_types,
                               empty_types, empty_types, empty_types, empty_types, NULL,
                               empty_types, NULL, NULL, tree_sitter_linkerscript},

    // CBM_LANG_GN
    [CBM_LANG_GN] = {CBM_LANG_GN, empty_types, empty_types, empty_types, gn_module_types,
                     empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                     empty_types, NULL, empty_types, NULL, NULL, tree_sitter_gn},

    // CBM_LANG_KCONFIG
    [CBM_LANG_KCONFIG] = {CBM_LANG_KCONFIG, empty_types, empty_types, empty_types,
                          kconfig_module_types, empty_types, empty_types, empty_types, empty_types,
                          empty_types, empty_types, empty_types, NULL, empty_types, NULL, NULL,
                          tree_sitter_kconfig},

    // CBM_LANG_BITBAKE
    [CBM_LANG_BITBAKE] = {CBM_LANG_BITBAKE, empty_types, empty_types, empty_types,
                          bitbake_module_types, empty_types, empty_types, empty_types, empty_types,
                          empty_types, empty_types, empty_types, NULL, empty_types, NULL, NULL,
                          tree_sitter_bitbake},

    // CBM_LANG_SMALI
    [CBM_LANG_SMALI] = {CBM_LANG_SMALI, empty_types, empty_types, empty_types, smali_module_types,
                        empty_types, empty_types, empty_types, empty_types, empty_types,
                        empty_types, empty_types, NULL, empty_types, NULL, NULL, tree_sitter_smali},

    // CBM_LANG_TABLEGEN
    [CBM_LANG_TABLEGEN] = {CBM_LANG_TABLEGEN, empty_types, empty_types, empty_types,
                           tablegen_module_types, empty_types, empty_types, empty_types,
                           empty_types, empty_types, empty_types, empty_types, NULL, empty_types,
                           NULL, NULL, tree_sitter_tablegen},

    // CBM_LANG_ISPC
    [CBM_LANG_ISPC] = {CBM_LANG_ISPC, empty_types, empty_types, empty_types, ispc_module_types,
                       empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                       empty_types, NULL, empty_types, NULL, NULL, tree_sitter_ispc},

    // CBM_LANG_CAIRO
    [CBM_LANG_CAIRO] = {CBM_LANG_CAIRO, empty_types, empty_types, empty_types, cairo_module_types,
                        empty_types, empty_types, empty_types, empty_types, empty_types,
                        empty_types, empty_types, NULL, empty_types, NULL, NULL, tree_sitter_cairo},

    // CBM_LANG_MOVE
    [CBM_LANG_MOVE] = {CBM_LANG_MOVE, empty_types, empty_types, empty_types, move_module_types,
                       empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                       empty_types, NULL, empty_types, NULL, NULL, tree_sitter_move},

    // CBM_LANG_SQUIRREL
    [CBM_LANG_SQUIRREL] = {CBM_LANG_SQUIRREL, empty_types, empty_types, empty_types,
                           squirrel_module_types, empty_types, empty_types, empty_types,
                           empty_types, empty_types, empty_types, empty_types, NULL, empty_types,
                           NULL, NULL, tree_sitter_squirrel},

    // CBM_LANG_FUNC
    [CBM_LANG_FUNC] = {CBM_LANG_FUNC, empty_types, empty_types, empty_types, func_module_types,
                       empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                       empty_types, NULL, empty_types, NULL, NULL, tree_sitter_func},

    // CBM_LANG_REGEX
    [CBM_LANG_REGEX] = {CBM_LANG_REGEX, empty_types, empty_types, empty_types, regex_module_types,
                        empty_types, empty_types, empty_types, empty_types, empty_types,
                        empty_types, empty_types, NULL, empty_types, NULL, NULL, tree_sitter_regex},

    // CBM_LANG_JSDOC
    [CBM_LANG_JSDOC] = {CBM_LANG_JSDOC, empty_types, empty_types, empty_types, jsdoc_module_types,
                        empty_types, empty_types, empty_types, empty_types, empty_types,
                        empty_types, empty_types, NULL, empty_types, NULL, NULL, tree_sitter_jsdoc},

    // CBM_LANG_RST
    [CBM_LANG_RST] = {CBM_LANG_RST, empty_types, empty_types, empty_types, rst_module_types,
                      empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                      empty_types, NULL, empty_types, NULL, NULL, tree_sitter_rst},

    // CBM_LANG_BEANCOUNT
    [CBM_LANG_BEANCOUNT] = {CBM_LANG_BEANCOUNT, empty_types, empty_types, empty_types,
                            beancount_module_types, empty_types, empty_types, empty_types,
                            empty_types, empty_types, empty_types, empty_types, NULL, empty_types,
                            NULL, NULL, tree_sitter_beancount},

    // CBM_LANG_MERMAID
    [CBM_LANG_MERMAID] = {CBM_LANG_MERMAID, empty_types, empty_types, empty_types,
                          mermaid_module_types, empty_types, empty_types, empty_types, empty_types,
                          empty_types, empty_types, empty_types, NULL, empty_types, NULL, NULL,
                          tree_sitter_mermaid},

    // CBM_LANG_PUPPET
    [CBM_LANG_PUPPET] = {CBM_LANG_PUPPET, empty_types, empty_types, empty_types,
                         puppet_module_types, empty_types, empty_types, empty_types, empty_types,
                         empty_types, empty_types, empty_types, NULL, empty_types, NULL, NULL,
                         tree_sitter_puppet},

    // CBM_LANG_PO
    [CBM_LANG_PO] = {CBM_LANG_PO, empty_types, empty_types, empty_types, po_module_types,
                     empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                     empty_types, NULL, empty_types, NULL, NULL, tree_sitter_po},

    // CBM_LANG_GITATTRIBUTES
    [CBM_LANG_GITATTRIBUTES] = {CBM_LANG_GITATTRIBUTES, empty_types, empty_types, empty_types,
                                gitattributes_module_types, empty_types, empty_types, empty_types,
                                empty_types, empty_types, empty_types, empty_types, NULL,
                                empty_types, NULL, NULL, tree_sitter_gitattributes},

    // CBM_LANG_GITIGNORE
    [CBM_LANG_GITIGNORE] = {CBM_LANG_GITIGNORE, empty_types, empty_types, empty_types,
                            gitignore_module_types, empty_types, empty_types, empty_types,
                            empty_types, empty_types, empty_types, empty_types, NULL, empty_types,
                            NULL, NULL, tree_sitter_gitignore},

    // CBM_LANG_SLANG
    [CBM_LANG_SLANG] = {CBM_LANG_SLANG, empty_types, empty_types, empty_types, slang_module_types,
                        empty_types, empty_types, empty_types, empty_types, empty_types,
                        empty_types, empty_types, NULL, empty_types, NULL, NULL, tree_sitter_slang},

    // CBM_LANG_LLVM_IR
    [CBM_LANG_LLVM_IR] = {CBM_LANG_LLVM_IR, empty_types, empty_types, empty_types,
                          llvm_module_types, empty_types, empty_types, empty_types, empty_types,
                          empty_types, empty_types, empty_types, NULL, empty_types, NULL, NULL,
                          tree_sitter_llvm},

    // CBM_LANG_SMITHY
    [CBM_LANG_SMITHY] = {CBM_LANG_SMITHY, empty_types, empty_types, empty_types,
                         smithy_module_types, empty_types, empty_types, empty_types, empty_types,
                         empty_types, empty_types, empty_types, NULL, empty_types, NULL, NULL,
                         tree_sitter_smithy},

    // CBM_LANG_WIT
    [CBM_LANG_WIT] = {CBM_LANG_WIT, empty_types, empty_types, empty_types, wit_module_types,
                      empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                      empty_types, NULL, empty_types, NULL, NULL, tree_sitter_wit},

    // CBM_LANG_TLAPLUS
    [CBM_LANG_TLAPLUS] = {CBM_LANG_TLAPLUS, empty_types, empty_types, empty_types,
                          tlaplus_module_types, empty_types, empty_types, empty_types, empty_types,
                          empty_types, empty_types, empty_types, NULL, empty_types, NULL, NULL,
                          tree_sitter_tlaplus},

    // CBM_LANG_PKL
    [CBM_LANG_PKL] = {CBM_LANG_PKL, empty_types, empty_types, empty_types, pkl_module_types,
                      empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                      empty_types, NULL, empty_types, NULL, NULL, tree_sitter_pkl},

    // CBM_LANG_GOMOD
    [CBM_LANG_GOMOD] = {CBM_LANG_GOMOD, empty_types, empty_types, empty_types, gomod_module_types,
                        empty_types, empty_types, empty_types, empty_types, empty_types,
                        empty_types, empty_types, NULL, empty_types, NULL, NULL, tree_sitter_gomod},

    // CBM_LANG_APEX
    [CBM_LANG_APEX] = {CBM_LANG_APEX, empty_types, empty_types, empty_types, apex_module_types,
                       empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                       empty_types, NULL, empty_types, NULL, NULL, tree_sitter_apex},

    // CBM_LANG_SOQL
    [CBM_LANG_SOQL] = {CBM_LANG_SOQL, empty_types, empty_types, empty_types, soql_module_types,
                       empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                       empty_types, NULL, empty_types, NULL, NULL, tree_sitter_soql},

    // CBM_LANG_SOSL
    [CBM_LANG_SOSL] = {CBM_LANG_SOSL, empty_types, empty_types, empty_types, sosl_module_types,
                       empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                       empty_types, NULL, empty_types, NULL, NULL, tree_sitter_sosl},

    // CBM_LANG_KUSTOMIZE — reuses YAML grammar; semantic extraction via cbm_extract_k8s()
    [CBM_LANG_KUSTOMIZE] = {CBM_LANG_KUSTOMIZE, empty_types, empty_types, empty_types,
                            yaml_module_types, empty_types, empty_types, empty_types, empty_types,
                            empty_types, empty_types, empty_types, NULL, empty_types, NULL, NULL,
                            tree_sitter_yaml},

    // CBM_LANG_K8S — reuses YAML grammar; semantic extraction via cbm_extract_k8s()
    [CBM_LANG_K8S] = {CBM_LANG_K8S, empty_types, empty_types, empty_types, yaml_module_types,
                      empty_types, empty_types, empty_types, empty_types, empty_types, empty_types,
                      empty_types, NULL, empty_types, NULL, NULL, tree_sitter_yaml},

};

_Static_assert(sizeof(lang_specs) / sizeof(lang_specs[0]) == CBM_LANG_COUNT,
               "lang_specs array size must match CBM_LANG_COUNT");

const CBMLangSpec *cbm_lang_spec(CBMLanguage lang) {
    if (lang < 0 || lang >= CBM_LANG_COUNT) {
        return NULL;
    }
    return &lang_specs[lang];
}

const TSLanguage *cbm_ts_language(CBMLanguage lang) {
    const CBMLangSpec *spec = cbm_lang_spec(lang);
    return spec && spec->ts_factory ? spec->ts_factory() : NULL;
}
