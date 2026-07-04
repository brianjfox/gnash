/* Copyright (c) 2026 Brian J. Fox
   Licensed under GPLv2 with the GPLv2-AI Exception. */

/* readline.h -- drop-in C interface to gnash's Readline reimplementation.
 *
 * In progress.  Present: the editing state, the bindable editing commands, the
 * keymap-driven dispatch loop, a termcap-based single-line redisplay, and the
 * interactive readline() entry point.  Coming: completion, incremental search,
 * vi mode, and .inputrc binding.
 */
#ifndef _READLINE_H_
#define _READLINE_H_

#include <stdio.h>

#include "readline/rltypedefs.h"
#include "readline/keymaps.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Editing state (mirrors readline's globals) ------------------------- */
extern char *rl_line_buffer;
extern int rl_point;
extern int rl_end;
extern int rl_mark;
extern int rl_done;

/* The prompt and the I/O streams readline uses. */
extern const char *rl_prompt;
extern FILE *rl_instream;
extern FILE *rl_outstream;

/* Numeric argument state. */
extern int rl_numeric_arg;
extern int rl_explicit_arg;

/* Nonzero after readline() returned because of end-of-file on an empty line. */
extern int rl_eof_found;

/* ---- Top-level entry points --------------------------------------------- */

/* Read a line interactively with PROMPT; returns a malloc'd line (without the
   trailing newline), or NULL on EOF with an empty line. */
extern char *readline (const char *prompt);

/* One-time initialization (keymaps, etc.).  Safe to call repeatedly. */
extern int rl_initialize (void);

/* Read a single key from rl_instream (EOF returns -1). */
extern int rl_read_key (void);

/* Repaint the current line. */
extern void rl_redisplay (void);

/* Erase the current input line (carriage-return + clear-to-end-of-line) so a
   caller can print a message where the prompt was, then rl_redisplay(). */
extern void rl_clear_current_line (void);

/* Called while readline is idle waiting for input (used for asynchronous
   notifications such as background-job completion).  NULL by default. */
extern rl_hook_func_t *rl_event_hook;

/* Optional syntax-highlighting hook: fills colors[len] with a per-character
   color id (0=none,1=green,2=red,3=yellow,4=cyan). */
extern void (*rl_highlight_function) (const char *line, int len, int *colors);

/* ---- Buffer primitives (text.c / util.c) -------------------------------- */
extern void rl_extend_line_buffer (int len);
extern int rl_insert_text (const char *text);
extern int rl_delete_text (int from, int to);
extern void rl_replace_line (const char *text, int clear_undo);
extern int rl_ding (void);
extern void rl_reset_last_command (void);

/* ---- Bindable editing commands ------------------------------------------ */
extern int rl_insert (int count, int c);
extern int rl_forward_char (int count, int key);
extern int rl_backward_char (int count, int key);
extern int rl_beg_of_line (int count, int key);
extern int rl_end_of_line (int count, int key);
extern int rl_forward_word (int count, int key);
extern int rl_backward_word (int count, int key);
extern int rl_delete (int count, int key);
extern int rl_rubout (int count, int key);
extern int rl_eof_or_delete (int count, int key);
extern int rl_kill_line (int count, int key);
extern int rl_backward_kill_line (int count, int key);
extern int rl_unix_line_discard (int count, int key);
extern int rl_kill_word (int count, int key);
extern int rl_backward_kill_word (int count, int key);
extern int rl_unix_word_rubout (int count, int key);
extern int rl_yank (int count, int key);
extern int rl_transpose_chars (int count, int key);
extern int rl_upcase_word (int count, int key);
extern int rl_downcase_word (int count, int key);
extern int rl_capitalize_word (int count, int key);
extern int rl_newline (int count, int key);
extern int rl_get_previous_history (int count, int key);
extern int rl_get_next_history (int count, int key);
extern int rl_digit_argument (int count, int key);
extern int rl_universal_argument (int count, int key);

/* ---- Completion --------------------------------------------------------- */

/* If set, called first with (text, start, end); returns a match array or NULL.
   This is the primary hook the shell uses for programmable completion. */
extern rl_completion_func_t *rl_attempted_completion_function;

/* Per-match generator used when no attempted-completion hook applies. */
extern rl_compentry_func_t *rl_completion_entry_function;

/* Optional hook to display the list of matches. */
extern void (*rl_completion_display_matches_hook) (char **, int, int);

/* Word-break and quoting tunables. */
extern const char *rl_basic_word_break_characters;
extern char *rl_completer_word_break_characters;
extern const char *rl_basic_quote_characters;
extern char *rl_completer_quote_characters;
extern char *rl_filename_quote_characters;

/* Behavioural tunables. */
extern int rl_completion_append_character;   /* appended after a sole match  */
extern int rl_completion_suppress_append;
extern int rl_completion_query_items;        /* ask before listing > N        */
extern int rl_ignore_completion_duplicates;
extern int rl_completion_type;               /* how completion was invoked    */
extern int rl_attempted_completion_over;     /* hook sets: don't fall back    */
extern int rl_filename_completion_desired;
extern int rl_completion_found_quote;
extern int rl_completion_mark_symlink_dirs;

/* Build a NUL-terminated match array by calling GEN(text, state=0,1,...); the
   first element is the longest common prefix of the matches. */
extern char **rl_completion_matches (const char *text, rl_compentry_func_t *gen);

/* Standard generators. */
extern char *rl_filename_completion_function (const char *text, int state);
extern char *rl_username_completion_function (const char *text, int state);

/* Bindable completion commands. */
extern int rl_complete (int count, int key);
extern int rl_possible_completions (int count, int key);
extern int rl_insert_completions (int count, int key);

/* ---- Incremental search ------------------------------------------------- */
extern int rl_reverse_search_history (int count, int key);
extern int rl_forward_search_history (int count, int key);

/* ---- inputrc / binding -------------------------------------------------- */

/* Bind KEY (or a "\C-x"-style key sequence) to a named or given function. */
extern int rl_bind_key (int key, rl_command_func_t *function);
extern int rl_bind_keyseq (const char *keyseq, rl_command_func_t *function);
extern rl_command_func_t *rl_named_function (const char *name);
extern const char **rl_funmap_names (void);

/* Parse one inputrc line (a binding or `set var value`). */
extern int rl_parse_and_bind (char *line);

/* Read an inputrc file (NULL -> $INPUTRC or ~/.inputrc). */
extern int rl_read_init_file (const char *filename);

/* ---- vi mode ------------------------------------------------------------ */
extern int rl_editing_mode;   /* 1 = emacs (default), 0 = vi */
extern int rl_vi_editing_mode (int count, int key);
extern int rl_emacs_editing_mode (int count, int key);
extern Keymap vi_insertion_keymap;
extern Keymap vi_movement_keymap;

#ifdef __cplusplus
}
#endif

#endif /* _READLINE_H_ */
