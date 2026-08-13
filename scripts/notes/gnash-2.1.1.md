# gnash 2.1.1

An interactive line-editing release: long lines now wrap, interactive
shells default to emacs mode, and a couple of editing/expansion edge
cases are brought in line with bash.

## New

- The line editor wraps an input line wider than the screen onto extra
  rows, as GNU Readline does, instead of scrolling it horizontally on a
  single row.  The previous single-row scrolling is still available via
  the `horizontal-scroll-mode` variable (`bind 'set horizontal-scroll-mode on'`).

## Fixes

- Interactive shells now default to emacs editing mode, matching bash
  (an explicit `-o vi`, `+o emacs`, `$SHELLOPTS`, or a startup file's
  `set -o vi` is still honored).  Among other things this lets a colored
  prompt's invisible `\[ \]` regions be measured correctly, so the cursor
  lands in the right column.
- At a fresh prompt, `C-n` (next-history) no longer clears the typed
  line; with no newer entry to move to it now does nothing, as bash
  does.  Stepping down from an actual history entry still restores the
  line you were typing.
- Pattern substitution with an invalid multibyte pattern
  (`${v//$'\xC3'/X}` in a UTF-8 locale) now matches bash's byte-wise
  fallback instead of leaving the value unchanged.
