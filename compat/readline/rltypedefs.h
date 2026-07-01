/* rltypedefs.h -- function-pointer typedefs for the Readline API (subset). */
#ifndef _RL_TYPEDEFS_H_
#define _RL_TYPEDEFS_H_

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#if !defined (_RL_FUNCTION_TYPEDEF)
#  define _RL_FUNCTION_TYPEDEF

/* Bindable command: (count, invoking-key) -> status (0 = ok). */
typedef int rl_command_func_t (int, int);

/* Hook and input function types. */
typedef int rl_hook_func_t (void);
typedef int rl_getc_func_t (FILE *);

/* Completion system. */
typedef char *rl_compentry_func_t (const char *, int);
typedef char **rl_completion_func_t (const char *, int, int);

/* Generic buffer+index predicate. */
typedef int rl_linebuf_func_t (char *, int);

#endif /* _RL_FUNCTION_TYPEDEF */

#ifdef __cplusplus
}
#endif

#endif /* _RL_TYPEDEFS_H_ */
