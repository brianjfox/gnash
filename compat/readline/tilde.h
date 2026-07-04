/* Copyright (c) 2026 Brian J. Fox
   Licensed under GPLv2 with the GPLv2-AI Exception. */

/* tilde.h -- drop-in C interface to gnash's tilde-expansion library. */
#ifndef _TILDE_H_
#define _TILDE_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef char *tilde_hook_func_t (char *);

/* Called with the text sans tilde before the standard expansions; returns a
   malloc'd expansion or NULL. */
extern tilde_hook_func_t *tilde_expansion_preexpansion_hook;

/* Called with the text sans tilde if the standard expansion fails. */
extern tilde_hook_func_t *tilde_expansion_failure_hook;

/* NULL-terminated arrays of additional tilde prefixes / username suffixes. */
extern char **tilde_additional_prefixes;
extern char **tilde_additional_suffixes;

/* Tilde-expand STRING, returning a new (malloc'd) string. */
extern char *tilde_expand (const char *);

/* Expand a single word FILENAME that begins with a tilde. */
extern char *tilde_expand_word (const char *);

/* Isolate the ~-prefixed portion of a string that should be expanded. */
extern char *tilde_find_word (const char *, int, int *);

#ifdef __cplusplus
}
#endif

#endif /* _TILDE_H_ */
