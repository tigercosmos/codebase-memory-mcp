/*
 * str_util.h — Safe string operations.
 *
 * All functions that return char* allocate via the provided arena
 * (no malloc, no free needed).
 */
#ifndef CBM_STR_UTIL_H
#define CBM_STR_UTIL_H

#include "arena.h"
#include <stdbool.h>
#include <stddef.h>

/* Join two path components with '/'. Handles trailing/leading slashes. */
char *cbm_path_join(CBMArena *a, const char *base, const char *name);

/* Join N path components. parts is an array of N strings. */
char *cbm_path_join_n(CBMArena *a, const char **parts, int n);

/* Get the file extension (without dot). Returns "" if none. */
const char *cbm_path_ext(const char *path);

/* Get the base name (after last '/'). Returns path if no '/'. */
const char *cbm_path_base(const char *path);

/* Get the directory part (before last '/'). Returns "." if no '/'. */
char *cbm_path_dir(CBMArena *a, const char *path);

/* Check if string starts with prefix. */
bool cbm_str_starts_with(const char *s, const char *prefix);

/* Check if string ends with suffix. */
bool cbm_str_ends_with(const char *s, const char *suffix);

/* Check if string contains substring. */
bool cbm_str_contains(const char *s, const char *sub);

/* Convert to lowercase (arena-allocated copy). */
char *cbm_str_tolower(CBMArena *a, const char *s);

/* Replace all occurrences of 'from' char with 'to' char (arena copy). */
char *cbm_str_replace_char(CBMArena *a, const char *s, char from, char to);

/* Strip file extension: "foo.go" → "foo" (arena copy). */
char *cbm_str_strip_ext(CBMArena *a, const char *path);

/* Split string by delimiter. Returns arena-allocated array + count.
 * The array itself and all substrings are arena-allocated. */
char **cbm_str_split(CBMArena *a, const char *s, char delim, int *out_count);

/* Validate a string is safe for shell interpolation inside single quotes.
 * Rejects: ' ; | & $ ` \n \r \0 (embedded NULs via len check).
 * Returns true if safe, false if the string contains shell metacharacters. */
bool cbm_validate_shell_arg(const char *s);

#endif /* CBM_STR_UTIL_H */
