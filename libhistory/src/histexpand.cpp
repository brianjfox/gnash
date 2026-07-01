// histexpand.cpp -- history expansion (the `!' designators, `^old^new^', word
// designators and modifiers).
//
// Faithful port of bash 5.3 lib/readline/histexpand.c.  History state is read
// and mutated through the C++ History instance (default_history()); everything
// else is string manipulation preserved from the original so the observable
// behaviour matches.  Multibyte-specific branches from the original are elided
// for now (byte-oriented behaviour), to be revisited alongside libreadline.

#include <cstdlib>
#include <cstring>

#include "gnash/history.hpp"
#include "gnash/sh/quote.hpp"
#include "gnash/sh/xmalloc.hpp"
#include "readline/history.h"

using gnash::history::Entry;
using gnash::history::default_history;
using gnash::sh::savestring;
using gnash::sh::single_quote;
using gnash::sh::xfree;
using gnash::sh::xmalloc;
using gnash::sh::xrealloc;

// ---- small helpers from histlib.h / chardefs.h ----------------------------

#define HISTORY_WORD_DELIMITERS " \t\n;&()|<>"
#define HISTORY_QUOTE_CHARACTERS "\"'`"
#define HISTORY_EVENT_DELIMITERS "^$*%-"
#define slashify_in_quotes "\\`\"$"

namespace {

inline int member(int c, const char *s) {
  return (c && s) ? (std::strchr(s, c) != nullptr) : 0;
}
inline int whitespace(int c) { return c == ' ' || c == '\t'; }
inline int fielddelim(int c) { return whitespace(c) || c == '\n'; }
inline int digit_p(int c) { return c >= '0' && c <= '9'; }
inline int digit_value(int c) { return c - '0'; }
inline int isdigit_(int c) { return c >= '0' && c <= '9'; }
inline int streqn(const char *a, const char *b, size_t n) {
  return std::strncmp(a, b, n) == 0;
}
inline void freeif(void *p) { if (p) xfree(p); }

// Error types (histlib.h).
enum { EVENT_NOT_FOUND = 0, BAD_WORD_SPEC, SUBST_FAILED, BAD_MODIFIER, NO_PREV_SUBST };

// Search flags (histlib.h).
constexpr int NON_ANCHORED_SEARCH = 0;
constexpr int ANCHORED_SEARCH = 1;

char history_event_delimiter_chars[] = HISTORY_EVENT_DELIMITERS;

char error_pointer;

char *subst_lhs;
char *subst_rhs;
int subst_lhs_len;
int subst_rhs_len;

// The last string searched for by a !?string? search, and what it matched.
char *search_string;
char *search_match;

// Backward search from the current interactive offset, mutating it.
int hs_search(const char *s, int flags) {
  auto &h = default_history();
  return (flags == ANCHORED_SEARCH) ? h.search_prefix(s, -1) : h.search(s, -1);
}

// forward declarations of statics
char *get_history_word_specifier(const char *, char *, int *);
int history_tokenize_word(const char *, int);
char **history_tokenize_internal(const char *, int, int *);
char *history_substring(const char *, int, int);
void freewords(char **, int);
char *history_find_word(const char *, int);
char *quote_breaks(char *);

}  // namespace

// ---- exported expansion tunables ------------------------------------------
extern "C" {
char history_expansion_char = '!';
char history_subst_char = '^';
char *history_no_expand_chars = const_cast<char *>(" \t\n\r=");
int history_quotes_inhibit_expansion = 0;
char *history_word_delimiters = const_cast<char *>(HISTORY_WORD_DELIMITERS);
int history_quoting_state = 0;
char *history_search_delimiter_chars = nullptr;
rl_linebuf_func_t *history_inhibit_expansion_function = nullptr;
}

// ---------------------------------------------------------------------------
// get_history_event
// ---------------------------------------------------------------------------
extern "C" char *get_history_event(const char *string, int *caller_index,
                                   int delimiting_quote) {
  int i, c;
  Entry *entry;
  int which, sign, local_index, substring_okay;
  int search_flags, old_offset;
  char *temp;
  auto &h = default_history();

  i = *caller_index;

  if (string[i] != history_expansion_char) return nullptr;
  i++;  // move past the expansion char

  sign = 1;
  substring_okay = 0;

#define RETURN_ENTRY(e, w) return ((e = h.get(w)) ? e->line : nullptr)

  // !! -- the previous command.
  if (string[i] == history_expansion_char) {
    i++;
    which = h.base() + (h.length() - 1);
    *caller_index = i;
    RETURN_ENTRY(entry, which);
  }

  // !-n
  if (string[i] == '-' && digit_p(string[i + 1])) {
    sign = -1;
    i++;
  }

  // !n
  if (digit_p(string[i])) {
    for (which = 0; digit_p(string[i]); i++)
      which = (which * 10) + digit_value(string[i]);
    *caller_index = i;
    if (sign < 0) which = (h.length() + h.base()) - which;
    RETURN_ENTRY(entry, which);
  }

  // Otherwise a search: `?str[?]` (anywhere) or `str` (prefix).
  if (string[i] == '?') {
    substring_okay++;
    i++;
  }

  for (local_index = i; (c = string[i]); i++) {
    if ((!substring_okay &&
         (whitespace(c) || c == ':' ||
          (i > local_index && c == '-') ||
          (c != '-' && member(c, history_event_delimiter_chars)) ||
          (history_search_delimiter_chars &&
           member(c, history_search_delimiter_chars)) ||
          string[i] == delimiting_quote)) ||
        string[i] == '\n' || (substring_okay && string[i] == '?'))
      break;
  }

  which = i - local_index;
  temp = static_cast<char *>(xmalloc(1 + which));
  if (which) std::strncpy(temp, string + local_index, static_cast<size_t>(which));
  temp[which] = '\0';

  if (substring_okay && string[i] == '?') i++;

  *caller_index = i;

  old_offset = h.offset();
#define FAIL_SEARCH()                            \
  do {                                           \
    h.set_offset(old_offset);                    \
    xfree(temp);                                 \
    return nullptr;                              \
  } while (0)

  // Empty search string reuses the previous !?string? search string.
  if (*temp == '\0' && substring_okay) {
    if (search_string) {
      xfree(temp);
      temp = savestring(search_string);
    } else {
      FAIL_SEARCH();
    }
  }

  search_flags = substring_okay ? NON_ANCHORED_SEARCH : ANCHORED_SEARCH;
  while (1) {
    local_index = hs_search(temp, search_flags);
    if (local_index < 0) FAIL_SEARCH();

    if (local_index == 0 || substring_okay) {
      entry = h.current();
      if (entry == nullptr) FAIL_SEARCH();
      h.set_offset(old_offset);

      if (substring_okay) {
        freeif(search_string);
        search_string = temp;
        freeif(search_match);
        search_match = history_find_word(entry->line, local_index);
      } else {
        xfree(temp);
      }
      return entry->line;
    }

    if (h.offset())
      h.set_offset(h.offset() - 1);
    else
      FAIL_SEARCH();
  }
#undef FAIL_SEARCH
#undef RETURN_ENTRY
}

namespace {

// Consume a single-quoted run: on entry *sindex is just past the opening
// quote; on exit it points at the closing quote.
void hist_string_extract_single_quoted(const char *string, int *sindex, int flags) {
  int i;
  for (i = *sindex; string[i] && string[i] != '\''; i++) {
    if ((flags & 1) && string[i] == '\\' && string[i + 1]) i++;
  }
  *sindex = i;
}

char *quote_breaks(char *s) {
  char *p, *r, *ret;
  int len = 3;

  for (p = s; p && *p; p++, len++) {
    if (*p == '\'')
      len += 3;
    else if (whitespace(*p) || *p == '\n')
      len += 2;
  }

  r = ret = static_cast<char *>(xmalloc(static_cast<size_t>(len)));
  *r++ = '\'';
  for (p = s; p && *p;) {
    if (*p == '\'') {
      *r++ = '\'';
      *r++ = '\\';
      *r++ = '\'';
      *r++ = '\'';
      p++;
    } else if (whitespace(*p) || *p == '\n') {
      *r++ = '\'';
      *r++ = *p++;
      *r++ = '\'';
    } else {
      *r++ = *p++;
    }
  }
  *r++ = '\'';
  *r = '\0';
  return ret;
}

char *hist_error(const char *s, int start, int current, int errtype) {
  char *temp;
  const char *emsg;
  int ll, elen;

  ll = current - start;

  switch (errtype) {
    case EVENT_NOT_FOUND: emsg = "event not found"; elen = 15; break;
    case BAD_WORD_SPEC:   emsg = "bad word specifier"; elen = 18; break;
    case SUBST_FAILED:    emsg = "substitution failed"; elen = 19; break;
    case BAD_MODIFIER:    emsg = "unrecognized history modifier"; elen = 29; break;
    case NO_PREV_SUBST:   emsg = "no previous substitution"; elen = 24; break;
    default:              emsg = "unknown expansion error"; elen = 23; break;
  }

  temp = static_cast<char *>(xmalloc(static_cast<size_t>(ll + elen + 3)));
  if (s[start])
    std::strncpy(temp, s + start, static_cast<size_t>(ll));
  else
    ll = 0;
  temp[ll] = ':';
  temp[ll + 1] = ' ';
  std::strcpy(temp + ll + 2, emsg);
  return temp;
}

// Extract a substitution pattern from STR at *IPTR up to DELIMITER.
char *get_subst_pattern(const char *str, int *iptr, int delimiter, int is_rhs,
                        int *lenptr) {
  int si, i, j, k;
  char *s = nullptr;

  i = *iptr;

  for (si = i; str[si] && str[si] != delimiter; si++)
    if (str[si] == '\\' && str[si + 1] == delimiter) si++;

  if (si > i || is_rhs) {
    s = static_cast<char *>(xmalloc(static_cast<size_t>(si - i + 1)));
    for (j = 0, k = i; k < si; j++, k++) {
      if (str[k] == '\\' && str[k + 1] == delimiter) k++;
      s[j] = str[k];
    }
    s[j] = '\0';
    if (lenptr) *lenptr = j;
  }

  i = si;
  if (str[i]) i++;
  *iptr = i;
  return s;
}

// Expand `&' in the rhs of a substitution to the lhs.
void postproc_subst_rhs() {
  char *nw;
  int i, j, new_size;

  nw = static_cast<char *>(xmalloc(static_cast<size_t>(new_size = subst_rhs_len + subst_lhs_len)));
  for (i = j = 0; i < subst_rhs_len; i++) {
    if (subst_rhs[i] == '&') {
      if (j + subst_lhs_len >= new_size)
        nw = static_cast<char *>(xrealloc(nw, static_cast<size_t>(new_size = new_size * 2 + subst_lhs_len)));
      std::strcpy(nw + j, subst_lhs);
      j += subst_lhs_len;
    } else {
      if (subst_rhs[i] == '\\' && subst_rhs[i + 1] == '&') i++;
      if (j + 1 >= new_size)
        nw = static_cast<char *>(xrealloc(nw, static_cast<size_t>(new_size *= 2)));
      nw[j++] = subst_rhs[i];
    }
  }
  nw[j] = '\0';
  xfree(subst_rhs);
  subst_rhs = nw;
  subst_rhs_len = j;
}

// Expand the bulk of a history specifier starting at STRING[START].
// Returns 0 on success, -1 on error, 1 for the `p' (print-only) modifier.
int history_expand_internal(const char *string, int start, int qc,
                            int *end_index_ptr, char **ret_string,
                            char *current_line) {
  int i, n;
  int substitute_globally, subst_bywords, want_quotes, print_only;
  char *event, *temp, *result, *tstr, *t, c, *word_spec;
  size_t result_len;
  int starting_index;

  result = static_cast<char *>(xmalloc(result_len = 128));
  i = start;

  // !:word or !$ etc. imply !! as the event.
  if (member(string[i + 1], ":$*%^")) {
    char fake_s[3];
    int fake_i = 0;
    i++;
    fake_s[0] = fake_s[1] = history_expansion_char;
    fake_s[2] = '\0';
    event = get_history_event(fake_s, &fake_i, 0);
  } else if (string[i + 1] == '#') {
    i += 2;
    event = current_line;
  } else {
    event = get_history_event(string, &i, qc);
  }

  if (event == nullptr) {
    *ret_string = hist_error(string, start, i, EVENT_NOT_FOUND);
    xfree(result);
    return -1;
  }

  // Word specifier?
  starting_index = i;
  word_spec = get_history_word_specifier(string, event, &i);
  if (word_spec == &error_pointer) {
    *ret_string = hist_error(string, starting_index, i, BAD_WORD_SPEC);
    xfree(result);
    return -1;
  }

  temp = word_spec ? savestring(word_spec) : savestring(event);
  freeif(word_spec);

  // Modifiers.
  want_quotes = substitute_globally = subst_bywords = print_only = 0;
  starting_index = i;

  while (string[i] == ':') {
    c = string[i + 1];

    if (c == 'g' || c == 'a') {
      substitute_globally = 1;
      i++;
      c = string[i + 1];
    } else if (c == 'G') {
      subst_bywords = 1;
      i++;
      c = string[i + 1];
    }

    switch (c) {
      default:
        *ret_string = hist_error(string, i + 1, i + 2, BAD_MODIFIER);
        xfree(result);
        xfree(temp);
        return -1;

      case 'q': want_quotes = 'q'; break;
      case 'x': want_quotes = 'x'; break;
      case 'p': print_only = 1; break;

      case 't':  // tail of pathname
        tstr = std::strrchr(temp, '/');
        if (tstr) {
          tstr++;
          t = savestring(tstr);
          xfree(temp);
          temp = t;
        }
        break;

      case 'h':  // head of pathname
        tstr = std::strrchr(temp, '/');
        if (tstr) *tstr = '\0';
        break;

      case 'r':  // remove suffix
        tstr = std::strrchr(temp, '.');
        if (tstr) *tstr = '\0';
        break;

      case 'e':  // suffix only
        tstr = std::strrchr(temp, '.');
        if (tstr) {
          t = savestring(tstr);
          xfree(temp);
          temp = t;
        }
        break;

      case '&':
      case 's': {
        char *new_event;
        int delimiter, failed, si, l_temp, we;

        delimiter = 0;
        if (c == 's') {
          if (i + 2 < static_cast<int>(std::strlen(string)))
            delimiter = string[i + 2];
          else
            break;  // no search delimiter

          i += 3;

          t = get_subst_pattern(string, &i, delimiter, 0, &subst_lhs_len);
          if (t) {
            freeif(subst_lhs);
            subst_lhs = t;
          } else if (!subst_lhs) {
            if (search_string && *search_string) {
              subst_lhs = savestring(search_string);
              subst_lhs_len = static_cast<int>(std::strlen(subst_lhs));
            } else {
              subst_lhs = nullptr;
              subst_lhs_len = 0;
            }
          }

          freeif(subst_rhs);
          subst_rhs = get_subst_pattern(string, &i, delimiter, 1, &subst_rhs_len);

          if (subst_lhs && member('&', subst_rhs)) postproc_subst_rhs();
        } else {
          i += 2;
        }

        if (subst_lhs_len == 0) {
          *ret_string = hist_error(string, starting_index, i, NO_PREV_SUBST);
          xfree(result);
          xfree(temp);
          return -1;
        }

        l_temp = static_cast<int>(std::strlen(temp));
        if (subst_lhs_len > l_temp) {
          *ret_string = hist_error(string, starting_index, i, SUBST_FAILED);
          xfree(result);
          xfree(temp);
          return -1;
        }

        si = we = 0;
        for (failed = 1; (si + subst_lhs_len) <= l_temp; si++) {
          if (subst_bywords && si > we) {
            for (; temp[si] && fielddelim(temp[si]); si++)
              ;
            we = history_tokenize_word(temp, si);
          }

          if (streqn(temp + si, subst_lhs, static_cast<size_t>(subst_lhs_len))) {
            int len = subst_rhs_len - subst_lhs_len + l_temp;
            new_event = static_cast<char *>(xmalloc(static_cast<size_t>(1 + len)));
            std::strncpy(new_event, temp, static_cast<size_t>(si));
            std::strncpy(new_event + si, subst_rhs, static_cast<size_t>(subst_rhs_len));
            std::strncpy(new_event + si + subst_rhs_len, temp + si + subst_lhs_len,
                         static_cast<size_t>(l_temp - (si + subst_lhs_len)));
            new_event[len] = '\0';
            xfree(temp);
            temp = new_event;

            failed = 0;

            if (substitute_globally) {
              si += subst_rhs_len - 1;
              l_temp = static_cast<int>(std::strlen(temp));
              substitute_globally++;
              continue;
            } else if (subst_bywords) {
              si = we;
              l_temp = static_cast<int>(std::strlen(temp));
              continue;
            } else {
              break;
            }
          }
        }

        if (substitute_globally > 1) {
          substitute_globally = 0;
          continue;  // don't increment i
        }
        if (failed == 0) continue;  // don't increment i

        *ret_string = hist_error(string, starting_index, i, SUBST_FAILED);
        xfree(result);
        xfree(temp);
        return -1;
      }
    }
    i += 2;
  }
  --i;  // back up by one, per the original

  if (want_quotes) {
    char *x;
    if (want_quotes == 'q')
      x = single_quote(temp);
    else if (want_quotes == 'x')
      x = quote_breaks(temp);
    else
      x = savestring(temp);
    xfree(temp);
    temp = x;
  }

  n = static_cast<int>(std::strlen(temp));
  if (static_cast<size_t>(n) >= result_len)
    result = static_cast<char *>(xrealloc(result, static_cast<size_t>(n + 2)));
  std::strcpy(result, temp);
  xfree(temp);

  *end_index_ptr = i;
  *ret_string = result;
  return print_only;
}

}  // namespace

// ---------------------------------------------------------------------------
// history_expand
// ---------------------------------------------------------------------------

#define ADD_STRING(s)                                              \
  do {                                                             \
    size_t sl = std::strlen(s);                                    \
    j += static_cast<int>(sl);                                     \
    if (static_cast<size_t>(j) >= result_len) {                   \
      while (static_cast<size_t>(j) >= result_len) result_len += 128; \
      result = static_cast<char *>(xrealloc(result, result_len)); \
    }                                                              \
    std::strcpy(result + j - static_cast<int>(sl), s);            \
  } while (0)

#define ADD_CHAR(ch)                                               \
  do {                                                             \
    if (static_cast<size_t>(j + 1) >= result_len)                 \
      result = static_cast<char *>(xrealloc(result, result_len += 64)); \
    result[j++] = (ch);                                            \
    result[j] = '\0';                                             \
  } while (0)

extern "C" int history_expand(const char *hstring, char **output) {
  int j;
  int i, r, passc, cc, modified, eindex, only_printing, dquote, squote, flag;
  int l;
  char *string;
  size_t result_len;
  char *result;
  char *temp;

  if (output == nullptr) return 0;

  // A zero expansion char disables all history expansion.
  if (history_expansion_char == 0) {
    *output = savestring(hstring);
    return 0;
  }

  result = static_cast<char *>(xmalloc(result_len = 256));
  result[0] = '\0';

  only_printing = modified = 0;
  l = static_cast<int>(std::strlen(hstring));

  // Quick substitution: "^this^that^" == "!!:s^this^that^".
  if ((history_quoting_state != '\'' || history_quotes_inhibit_expansion == 0) &&
      hstring[0] == history_subst_char) {
    string = static_cast<char *>(xmalloc(static_cast<size_t>(l + 5)));
    string[0] = string[1] = history_expansion_char;
    string[2] = ':';
    string[3] = 's';
    std::strcpy(string + 4, hstring);
    l += 4;
  } else {
    string = const_cast<char *>(hstring);

    dquote = history_quoting_state == '"';
    squote = history_quoting_state == '\'';

    i = 0;
    if (squote && history_quotes_inhibit_expansion) {
      hist_string_extract_single_quoted(string, &i, 0);
      squote = 0;
      if (string[i]) i++;
      if (i >= l) i = l;
    }

    for (; string[i]; i++) {
      cc = string[i + 1];
      if (history_comment_char && string[i] == history_comment_char &&
          dquote == 0 &&
          (i == 0 || member(string[i - 1], history_word_delimiters))) {
        while (string[i]) i++;
        break;
      } else if (string[i] == history_expansion_char) {
        if (cc == 0 || member(cc, history_no_expand_chars))
          continue;
        else if (dquote && cc == '"')
          continue;
        else if (history_inhibit_expansion_function &&
                 (*history_inhibit_expansion_function)(string, i))
          continue;
        else
          break;
      } else if (dquote && string[i] == '\\' && cc == '"') {
        i++;
      } else if (history_quotes_inhibit_expansion && string[i] == '"') {
        dquote = 1 - dquote;
      } else if (dquote == 0 && history_quotes_inhibit_expansion &&
                 string[i] == '\'') {
        flag = (i > 0 && string[i - 1] == '$');
        i++;
        hist_string_extract_single_quoted(string, &i, flag);
        if (i >= l) {
          i = l;
          break;
        }
      } else if (history_quotes_inhibit_expansion && string[i] == '\\') {
        if (cc == '\'' || cc == history_expansion_char) i++;
      }
    }

    if (string[i] != history_expansion_char) {
      xfree(result);
      *output = savestring(string);
      return 0;
    }
  }

  // Perform the substitution.
  dquote = history_quoting_state == '"';
  squote = history_quoting_state == '\'';

  i = j = 0;
  if (squote && history_quotes_inhibit_expansion) {
    int c;
    hist_string_extract_single_quoted(string, &i, 0);
    squote = 0;
    for (c = 0; c < i; c++) ADD_CHAR(string[c]);
    if (string[i]) {
      ADD_CHAR(string[i]);
      i++;
    }
  }

  for (passc = 0; i < l; i++) {
    int qc, tchar = string[i];

    if (passc) {
      passc = 0;
      ADD_CHAR(tchar);
      continue;
    }

    if (tchar == history_expansion_char)
      tchar = -3;
    else if (tchar == history_comment_char)
      tchar = -2;

    switch (tchar) {
      default:
        ADD_CHAR(string[i]);
        break;

      case '\\':
        passc++;
        ADD_CHAR(tchar);
        break;

      case '"':
        dquote = 1 - dquote;
        ADD_CHAR(tchar);
        break;

      case '\'': {
        if (squote) {
          squote = 0;
          ADD_CHAR(tchar);
        } else if (dquote == 0 && history_quotes_inhibit_expansion) {
          int quote, slen;
          flag = (i > 0 && string[i - 1] == '$');
          quote = i++;
          hist_string_extract_single_quoted(string, &i, flag);
          slen = i - quote + 2;
          temp = static_cast<char *>(xmalloc(static_cast<size_t>(slen)));
          std::strncpy(temp, string + quote, static_cast<size_t>(slen));
          temp[slen - 1] = '\0';
          ADD_STRING(temp);
          xfree(temp);
        } else if (dquote == 0 && squote == 0 &&
                   history_quotes_inhibit_expansion == 0) {
          squote = 1;
          ADD_CHAR(string[i]);
        } else {
          ADD_CHAR(string[i]);
        }
        break;
      }

      case -2:  // history_comment_char
        if ((dquote == 0 || history_quotes_inhibit_expansion == 0) &&
            (i == 0 || member(string[i - 1], history_word_delimiters))) {
          temp = static_cast<char *>(xmalloc(static_cast<size_t>(l - i + 1)));
          std::strcpy(temp, string + i);
          ADD_STRING(temp);
          xfree(temp);
          i = l;
        } else {
          ADD_CHAR(string[i]);
        }
        break;

      case -3:  // history_expansion_char
        cc = string[i + 1];

        if (cc == 0 || member(cc, history_no_expand_chars) || (dquote && cc == '"')) {
          ADD_CHAR(string[i]);
          break;
        }

        if (history_inhibit_expansion_function) {
          int save_j = j;
          int inhibit;
          ADD_CHAR(string[i]);
          ADD_CHAR(cc);
          inhibit = (*history_inhibit_expansion_function)(result, save_j);
          if (inhibit) {
            result[--j] = '\0';  // un-add cc
            break;
          } else {
            result[j = save_j] = '\0';
          }
        }

        qc = squote ? '\'' : (dquote ? '"' : 0);
        r = history_expand_internal(string, i, qc, &eindex, &temp, result);
        if (r < 0) {
          *output = temp;
          xfree(result);
          if (string != hstring) xfree(string);
          return -1;
        } else {
          if (temp) {
            modified++;
            if (*temp) ADD_STRING(temp);
            xfree(temp);
          }
          only_printing += r == 1;
          i = eindex;
        }
        break;
    }
  }

  *output = result;
  if (string != hstring) xfree(string);

  if (only_printing) return 2;
  return modified != 0;
}

#undef ADD_STRING
#undef ADD_CHAR

// ---------------------------------------------------------------------------
// Word specifiers, argument extraction, tokenizing.
// ---------------------------------------------------------------------------

namespace {

char *get_history_word_specifier(const char *spec, char *from, int *caller_index) {
  int i = *caller_index;
  int first, last;
  int expecting_word_spec = 0;
  char *result = nullptr;

  first = last = 0;

  if (spec[i] == ':') {
    i++;
    expecting_word_spec++;
  }

  if (spec[i] == '%') {  // last word searched for
    *caller_index = i + 1;
    return search_match ? savestring(search_match) : savestring("");
  }

  if (spec[i] == '*') {  // all args but the command
    *caller_index = i + 1;
    result = history_arg_extract(1, '$', from);
    return result ? result : savestring("");
  }

  if (spec[i] == '$') {  // last arg
    *caller_index = i + 1;
    return history_arg_extract('$', '$', from);
  }

  if (spec[i] == '-')
    first = 0;
  else if (spec[i] == '^') {
    first = 1;
    i++;
  } else if (digit_p(spec[i]) && expecting_word_spec) {
    for (first = 0; digit_p(spec[i]); i++)
      first = (first * 10) + digit_value(spec[i]);
  } else {
    return nullptr;  // no valid `first'
  }

  if (spec[i] == '^' || spec[i] == '*') {
    last = (spec[i] == '^') ? 1 : '$';
    i++;
  } else if (spec[i] != '-') {
    last = first;
  } else {
    i++;
    if (digit_p(spec[i])) {
      for (last = 0; digit_p(spec[i]); i++)
        last = (last * 10) + digit_value(spec[i]);
    } else if (spec[i] == '$') {
      i++;
      last = '$';
    } else if (spec[i] == '^') {
      i++;
      last = 1;
    } else {
      last = -1;  // x- abbreviates x-$ omitting the last word
    }
  }

  *caller_index = i;

  if (last >= first || last == '$' || last < 0)
    result = history_arg_extract(first, last, from);

  return result ? result : &error_pointer;
}

}  // namespace

extern "C" char *history_arg_extract(int first, int last, const char *string) {
  int i, len;
  char *result;
  size_t size, offset;
  char **list;

  if ((list = history_tokenize(string)) == nullptr) return nullptr;

  for (len = 0; list[len]; len++)
    ;

  if (last < 0) last = len + last - 1;
  if (first < 0) first = len + first - 1;
  if (last == '$') last = len - 1;
  if (first == '$') first = len - 1;

  last++;

  if (first >= len || last > len || first < 0 || last < 0 || first > last) {
    result = nullptr;
  } else {
    for (size = 0, i = first; i < last; i++)
      size += std::strlen(list[i]) + 1;
    result = static_cast<char *>(xmalloc(size + 1));
    result[0] = '\0';

    for (i = first, offset = 0; i < last; i++) {
      std::strcpy(result + offset, list[i]);
      offset += std::strlen(list[i]);
      if (i + 1 < last) {
        result[offset++] = ' ';
        result[offset] = 0;
      }
    }
  }

  for (i = 0; i < len; i++) xfree(list[i]);
  xfree(list);
  return result;
}

namespace {

int history_tokenize_word(const char *string, int ind) {
  int i, j;
  int delimiter, nestdelim, delimopen;

  i = ind;
  delimiter = nestdelim = delimopen = 0;

  if (member(string[i], "()\n")) {
    i++;
    return i;
  }

  if (isdigit_(string[i])) {
    j = i;
    while (string[j] && isdigit_(string[j])) j++;
    if (string[j] == 0) return j;
    if (string[j] == '<' || string[j] == '>')
      i = j;  // file descriptor
    else {
      i = j;
      goto get_word;  // part of a word
    }
  }

  if (member(string[i], "<>;&|")) {
    int peek = string[i + 1];

    if (peek == string[i]) {
      if (peek == '<' && string[i + 2] == '-')
        i++;
      else if (peek == '<' && string[i + 2] == '<')
        i++;
      i += 2;
      return i;
    } else if (peek == '&' && (string[i] == '>' || string[i] == '<')) {
      j = i + 2;
      while (string[j] && isdigit_(string[j])) j++;
      if (string[j] == '-') j++;
      return j;
    } else if ((peek == '>' && string[i] == '&') ||
               (peek == '|' && string[i] == '>')) {
      i += 2;
      return i;
    } else if (peek == '(' && (string[i] == '>' || string[i] == '<')) {
      i += 2;
      delimopen = '(';
      delimiter = ')';
      nestdelim = 1;
      goto get_word;
    }

    i++;
    return i;
  }

get_word:
  if (delimiter == 0 && member(string[i], HISTORY_QUOTE_CHARACTERS))
    delimiter = string[i++];

  for (; string[i]; i++) {
    if (string[i] == '\\' && string[i + 1] == '\n') {
      i++;
      continue;
    }

    if (string[i] == '\\' && delimiter != '\'' &&
        (delimiter != '"' || member(string[i], slashify_in_quotes))) {
      i++;
      if (string[i] == 0) break;
      continue;
    }

    if (nestdelim && string[i] == delimopen) {
      nestdelim++;
      continue;
    }
    if (nestdelim && string[i] == delimiter) {
      nestdelim--;
      if (nestdelim == 0) delimiter = 0;
      continue;
    }

    if (delimiter && string[i] == delimiter) {
      delimiter = 0;
      continue;
    }

    if (nestdelim == 0 && delimiter == 0 && member(string[i], "<>$!@?+*") &&
        string[i + 1] == '(') {
      i++;
      if (string[i + 1] == 0) break;
      delimopen = '(';
      delimiter = ')';
      nestdelim = 1;
      continue;
    }

    if (delimiter == 0 && member(string[i], history_word_delimiters)) break;

    if (delimiter == 0 && member(string[i], HISTORY_QUOTE_CHARACTERS))
      delimiter = string[i];
  }

  return i;
}

char *history_substring(const char *string, int start, int end) {
  int len = end - start;
  char *result = static_cast<char *>(xmalloc(static_cast<size_t>(len + 1)));
  std::strncpy(result, string + start, static_cast<size_t>(len));
  result[len] = '\0';
  return result;
}

char **history_tokenize_internal(const char *string, int wind, int *indp) {
  char **result;
  int i, start, result_index;
  size_t size;

  if (indp && wind != -1) *indp = -1;

  for (i = result_index = 0, size = 0, result = nullptr; string[i];) {
    for (; string[i] && fielddelim(string[i]); i++)
      ;
    if (string[i] == 0 || string[i] == history_comment_char) return result;

    start = i;
    i = history_tokenize_word(string, start);

    if (i == start && history_word_delimiters) {
      i++;
      while (string[i] && member(string[i], history_word_delimiters)) i++;
    }

    if (indp && wind != -1 && wind >= start && wind < i) *indp = result_index;

    if (static_cast<size_t>(result_index + 2) >= size)
      result = static_cast<char **>(
          xrealloc(result, (size += 10) * sizeof(char *)));

    result[result_index++] = history_substring(string, start, i);
    result[result_index] = nullptr;
  }

  return result;
}

void freewords(char **words, int start) {
  for (int i = start; words[i]; i++) xfree(words[i]);
}

char *history_find_word(const char *line, int ind) {
  char **words, *s;
  int i, wind;

  words = history_tokenize_internal(line, ind, &wind);
  if (wind == -1 || words == nullptr) {
    if (words) freewords(words, 0);
    freeif(words);
    return nullptr;
  }
  s = words[wind];
  for (i = 0; i < wind; i++) xfree(words[i]);
  freewords(words, wind + 1);
  xfree(words);
  return s;
}

}  // namespace

extern "C" char **history_tokenize(const char *string) {
  return history_tokenize_internal(string, -1, nullptr);
}
