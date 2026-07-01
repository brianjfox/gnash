/* glob.h -- filename globbing (drop-in subset). */
#ifndef _GLOB_H_
#define _GLOB_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Flags for glob_filename(), matching bash's lib/glob/glob.h. */
#define GX_MARKDIRS  0x001  /* mark directory names with a trailing `/' */
#define GX_NOCASE    0x002  /* ignore case                              */
#define GX_MATCHDOT  0x004  /* match a leading `.' with wildcards       */
#define GX_MATCHDIRS 0x008  /* match only directory names               */
#define GX_ALLDIRS   0x010  /* match all directory names                */
#define GX_GLOBSTAR  0x400  /* enable ** recursive matching             */

/* Nonzero if PATTERN contains any unquoted globbing metacharacters. */
extern int glob_pattern_p (const char *pattern);

/* Expand PATHNAME; returns a NULL-terminated, malloc'd array of matches (each
   malloc'd), or NULL if there are no matches. */
extern char **glob_filename (char *pathname, int flags);

#ifdef __cplusplus
}
#endif

#endif /* _GLOB_H_ */
