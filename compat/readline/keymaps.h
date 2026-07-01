/* keymaps.h -- Readline keymaps (drop-in subset).
 *
 * Note: KEYMAP_ENTRY carries an explicit `kmap` pointer for ISKMAP entries
 * instead of overloading the function pointer (as classic readline does), so
 * gnash avoids casting between object and function pointers.  ISFUNC entries
 * use `function` exactly as before.
 */
#ifndef _KEYMAPS_H_
#define _KEYMAPS_H_

#include "readline/rltypedefs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _keymap_entry {
  char type;                     /* ISFUNC, ISKMAP, or ISMACR */
  rl_command_func_t *function;   /* for ISFUNC */
  void *kmap;                    /* for ISKMAP: the sub-keymap */
} KEYMAP_ENTRY;

#define KEYMAP_SIZE 257
#define ANYOTHERKEY (KEYMAP_SIZE - 1)

typedef KEYMAP_ENTRY KEYMAP_ENTRY_ARRAY[KEYMAP_SIZE];
typedef KEYMAP_ENTRY *Keymap;

/* Values for KEYMAP_ENTRY.type. */
#define ISFUNC 0
#define ISKMAP 1
#define ISMACR 2

extern Keymap emacs_standard_keymap, emacs_meta_keymap, emacs_ctlx_keymap;

extern Keymap rl_make_bare_keymap (void);
extern Keymap rl_copy_keymap (Keymap);
extern Keymap rl_make_keymap (void);
extern void rl_discard_keymap (Keymap);

extern Keymap rl_get_keymap (void);
extern void rl_set_keymap (Keymap);
extern Keymap rl_get_keymap_by_name (const char *);

#ifdef __cplusplus
}
#endif

#endif /* _KEYMAPS_H_ */
