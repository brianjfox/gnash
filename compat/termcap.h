/* Copyright (c) 2026 Brian J. Fox
   Licensed under GPLv2 with the GPLv2-AI Exception. */

/* termcap.h -- drop-in C interface to gnash's termcap work-alike. */
#ifndef _TERMCAP_H
#define _TERMCAP_H 1

#ifdef __cplusplus
extern "C" {
#endif

extern int tgetent (char *buffer, const char *termtype);

extern int tgetnum (const char *name);
extern int tgetflag (const char *name);
extern char *tgetstr (const char *name, char **area);

extern char PC;
extern short ospeed;
extern int tputs (const char *string, int nlines, int (*outfun) (int));

extern char *tparam (const char *ctlstring, char *buffer, int size, ...);

extern char *UP;
extern char *BC;

extern char *tgoto (const char *cstring, int hpos, int vpos);

#ifdef __cplusplus
}
#endif

#endif /* not _TERMCAP_H */
