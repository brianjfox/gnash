/* Copyright (c) 2026 Brian J. Fox
   Licensed under GPLv2 with the GPLv2-AI Exception. */

/* hist_entry.h -- the single, C-compatible definition of a history entry.
 *
 * Both the modern C++ API (<gnash/history.hpp>) and the drop-in C shim
 * (<readline/history.h>) alias this one POD so there is exactly one memory
 * layout for a history entry across the whole project.
 *
 * Layout is intentionally identical to GNU History's HIST_ENTRY.
 */
#ifndef GNASH_HIST_ENTRY_H
#define GNASH_HIST_ENTRY_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void *gnash_histdata_t;

typedef struct gnash_hist_entry {
  char *line;        /* the remembered line (xmalloc'd)                */
  char *timestamp;   /* "#<seconds>" form, or NULL (xmalloc'd)         */
  gnash_histdata_t data; /* opaque application data                    */
} gnash_hist_entry;

#ifdef __cplusplus
}
#endif

#endif /* GNASH_HIST_ENTRY_H */
