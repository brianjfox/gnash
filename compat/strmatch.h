/* Copyright (c) 2026 Brian J. Fox
   Licensed under GPLv2 with the GPLv2-AI Exception. */

/* strmatch.h -- ksh-like extended pattern matching (drop-in). */
#ifndef _STRMATCH_H
#define _STRMATCH_H 1

#ifdef __cplusplus
extern "C" {
#endif

/* Flags for strmatch(), matching bash's lib/glob/strmatch.h. */
#define FNM_PATHNAME    (1 << 0)  /* no wildcard matches `/'                */
#define FNM_NOESCAPE    (1 << 1)  /* backslashes don't quote                */
#define FNM_PERIOD      (1 << 2)  /* leading `.' matched only explicitly    */
#define FNM_LEADING_DIR (1 << 3)  /* ignore `/...' after a match            */
#define FNM_CASEFOLD    (1 << 4)  /* case-insensitive                       */
#define FNM_EXTMATCH    (1 << 5)  /* ksh extended matching ?(..) *(..) ...  */
#define FNM_FIRSTCHAR   (1 << 6)  /* match only the first character         */
#define FNM_DOTDOT      (1 << 7)  /* force `.'/`..' to match explicitly     */

#define FNM_NOMATCH     1         /* value returned when STRING doesn't match */

/* Match STRING against PATTERN, returning 0 on match, FNM_NOMATCH otherwise. */
extern int strmatch (char *pattern, char *string, int flags);

#ifdef __cplusplus
}
#endif

#endif /* _STRMATCH_H */
