/* history.h -- drop-in C interface to gnash's GNU History reimplementation.
 *
 * This is the compatibility shim: source that was written against GNU
 * History can include <readline/history.h> and link gnash_history for the
 * classic entry points.  The real implementation lives behind the C++ API in
 * <gnash/history.hpp>; these functions operate on a process-global instance.
 */
#ifndef _HISTORY_H_
#define _HISTORY_H_

#include <time.h>

#include "gnash/hist_entry.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef gnash_histdata_t histdata_t;
typedef struct gnash_hist_entry HIST_ENTRY;

#ifndef HIST_ENTRY_DEFINED
#  define HIST_ENTRY_DEFINED
#endif

/* State snapshot, matching GNU History's HISTORY_STATE. */
typedef struct _hist_state {
  HIST_ENTRY **entries;
  int offset;
  int length;
  int size;
  int flags;
} HISTORY_STATE;

#define HS_STIFLED 0x01

/* Signature of the optional history-expansion inhibitor hook. */
#ifndef _RL_LINEBUF_FUNC_T
#define _RL_LINEBUF_FUNC_T
typedef int rl_linebuf_func_t (char *, int);
#endif

/* Initialization and state. */
extern void using_history (void);
extern HISTORY_STATE *history_get_history_state (void);
extern void history_set_history_state (HISTORY_STATE *);

/* List management. */
extern void add_history (const char *);
extern void add_history_time (const char *);
extern HIST_ENTRY *remove_history (int);
extern HIST_ENTRY **remove_history_range (int, int);
extern HIST_ENTRY *alloc_history_entry (char *, char *);
extern HIST_ENTRY *copy_history_entry (HIST_ENTRY *);
extern histdata_t free_history_entry (HIST_ENTRY *);
extern HIST_ENTRY *replace_history_entry (int, const char *, histdata_t);
extern void clear_history (void);
extern void stifle_history (int);
extern int unstifle_history (void);
extern int history_is_stifled (void);

/* Information. */
extern HIST_ENTRY **history_list (void);
extern int where_history (void);
extern HIST_ENTRY *current_history (void);
extern HIST_ENTRY *history_get (int);
extern time_t history_get_time (HIST_ENTRY *);
extern int history_total_bytes (void);

/* Moving around. */
extern int history_set_pos (int);
extern HIST_ENTRY *previous_history (void);
extern HIST_ENTRY *next_history (void);

/* Searching. */
extern int history_search (const char *, int);
extern int history_search_prefix (const char *, int);
extern int history_search_pos (const char *, int, int);

/* History file. */
extern int read_history (const char *);
extern int read_history_range (const char *, int, int);
extern int write_history (const char *);
extern int append_history (int, const char *);
extern int history_truncate_file (const char *, int);

/* History expansion (see histexpand.cpp). */
extern int history_expand (const char *, char **);
extern char *get_history_event (const char *, int *, int);
extern char *history_arg_extract (int, int, const char *);
extern char **history_tokenize (const char *);

/* Exported variables. */
extern int history_base;
extern int history_length;
extern int history_max_entries;
extern int history_offset;
extern int history_lines_read_from_file;
extern int history_lines_written_to_file;
extern char history_comment_char;
extern int history_write_timestamps;
extern int max_input_history;

/* History expansion tunables (defined in histexpand.cpp). */
extern char history_expansion_char;
extern char history_subst_char;
extern char *history_word_delimiters;
extern char *history_no_expand_chars;
extern char *history_search_delimiter_chars;
extern int history_quotes_inhibit_expansion;
extern int history_quoting_state;
extern rl_linebuf_func_t *history_inhibit_expansion_function;

#ifdef __cplusplus
}
#endif

#endif /* !_HISTORY_H_ */
