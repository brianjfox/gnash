// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// smatch.cpp -- ksh-like extended pattern matching (strmatch).
//
// A byte-oriented port of bash 5.3 lib/glob/sm_loop.c (the single-byte
// instantiation from smatch.c): gmatch drives matching of `*`, `?`, `[...]`
// bracket expressions (with POSIX [:class:], [.sym.], [=equiv=]) and the ksh
// extended operators ?(..) *(..) +(..) @(..) !(..).  Locale collation is
// simplified to C/ASCII ordering; wide-character matching is a later addition.

#include <cctype>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>

#include "strmatch.h"

// Set from the shell's SIGINT handler (async-signal-safe: a single sig_atomic_t
// store) so a long-running match bails out promptly with FNM_NOMATCH.  File
// scope with C linkage so the setter is reachable from the shell core.
static volatile std::sig_atomic_t interrupt_state = 0;
static volatile std::sig_atomic_t terminating_signal = 0;

extern "C" void strmatch_set_interrupt(int state) { interrupt_state = state ? 1 : 0; }

namespace {

using UC = unsigned char;

// Byte width of the character at s (s < se) under the current locale; 1 in a
// unibyte/C locale or on an invalid/truncated sequence, so scanning never
// stalls.
inline size_t mb_clen(const UC *s, const UC *se) {
  if (MB_CUR_MAX <= 1 || s >= se) return 1;
  std::mbstate_t st{};
  size_t r = std::mbrtowc(nullptr, reinterpret_cast<const char *>(s),
                          static_cast<size_t>(se - s), &st);
  if (r == 0 || r == static_cast<size_t>(-1) || r == static_cast<size_t>(-2)) return 1;
  return r;
}

// Decode the character at s (s < se) to a wide code point, setting len to its
// byte width (>=1); falls back to the raw byte in a unibyte locale / on error.
inline std::wint_t mb_cp(const UC *s, const UC *se, size_t &len) {
  if (MB_CUR_MAX <= 1 || s >= se) { len = 1; return s < se ? *s : 0; }
  wchar_t wc = 0;
  std::mbstate_t st{};
  size_t r = std::mbrtowc(&wc, reinterpret_cast<const char *>(s),
                          static_cast<size_t>(se - s), &st);
  if (r == 0 || r == static_cast<size_t>(-1) || r == static_cast<size_t>(-2)) {
    len = 1;
    return *s;
  }
  len = r;
  return static_cast<std::wint_t>(wc);
}

struct SmEnds {
  UC *pattern;
  UC *string;
};

inline int fold(int c, int flags) {
  if ((flags & FNM_CASEFOLD) && std::isupper(static_cast<unsigned char>(c)))
    return std::tolower(static_cast<unsigned char>(c));
  return static_cast<unsigned char>(c);
}

inline bool pathsep(int c) { return c == '/' || c == 0; }

inline bool sdot_or_dotdot(const UC *s) {
  return s[0] == '.' &&
         (pathsep(s[1]) || (s[1] == '.' && pathsep(s[2])));
}

inline int rangecmp(int a, int b) {
  return static_cast<int>(static_cast<UC>(a)) - static_cast<int>(static_cast<UC>(b));
}

// A collating symbol [.sym.] -> a character.  Single-character symbols and a
// few common names are supported.
int collsym(const UC *p, int len) {
  if (len == 1) return p[0];
  struct {
    const char *name;
    int ch;
  } tbl[] = {{"NUL", 0},      {"newline", '\n'}, {"tab", '\t'},
             {"space", ' '},  {"period", '.'},   {"slash", '/'},
             {"hyphen", '-'}, {nullptr, 0}};
  for (int i = 0; tbl[i].name; i++)
    if (static_cast<int>(std::strlen(tbl[i].name)) == len &&
        std::strncmp(reinterpret_cast<const char *>(p), tbl[i].name,
                     static_cast<size_t>(len)) == 0)
      return tbl[i].ch;
  return p[0];
}

// POSIX [:class:] test.  Returns 1/0, or -1 for an unknown class.
// c is a wide code point so a bracket class ([[:alpha:]]) matches a multibyte
// character; the wide iswctype functions honor the locale for the whole range.
int is_cclass(std::wint_t c, const char *name) {
  struct {
    const char *name;
    int (*fn)(std::wint_t);
  } tbl[] = {{"alpha", std::iswalpha}, {"digit", std::iswdigit},
             {"alnum", std::iswalnum}, {"space", std::iswspace},
             {"upper", std::iswupper}, {"lower", std::iswlower},
             {"punct", std::iswpunct}, {"cntrl", std::iswcntrl},
             {"print", std::iswprint}, {"graph", std::iswgraph},
             {"blank", std::iswblank}, {"xdigit", std::iswxdigit},
             {"ascii", [](std::wint_t ch) { return static_cast<int>(ch <= 0x7f); }},
             {"word", [](std::wint_t ch) { return static_cast<int>(std::iswalnum(ch) || ch == L'_'); }},
             {nullptr, nullptr}};
  for (int i = 0; tbl[i].name; i++)
    if (std::strcmp(tbl[i].name, name) == 0)
      return tbl[i].fn(c) ? 1 : 0;
  return -1;
}

void dequote_pathname(UC *s) {
  UC *r = s;
  while (*s) {
    if (*s == '\\' && s[1]) s++;
    *r++ = *s++;
  }
  *r = '\0';
}

// Parse [.sym.] / [=c=] / [:class:] starting at P (the type char).  Returns the
// position of the terminating type char (before `]`), or NULL.
UC *parse_subbracket(UC *p, int flags) {
  UC type = *p;
  while (*++p != '\0' && !(p[0] == '/' && (flags & FNM_PATHNAME)) &&
         !(type != '.' && *p == ']')) {
    if (*p == type && p[1] == ']') return p;
  }
  return nullptr;
}

UC *patscan(UC *string, UC *end, int delim, int flags);
int gmatch(UC *string, UC *se, UC *pattern, UC *pe, SmEnds *ends, int flags);
int extmatch(int xc, UC *s, UC *se, UC *p, UC *pe, int flags);

// Match a bracket expression at P (P points just past `[`); returns the
// position after the expression on success, or NULL.
UC *brackmatch(UC *p, UC test_in, std::wint_t test_cp, int flags) {
  UC cstart, cend, c;
  int is_not, forcecoll, isrange;
  int pc;
  UC *savep, *close;
  UC orig_test = test_in;
  UC test = static_cast<UC>(fold(orig_test, flags));

  savep = p;

  is_not = (*p == '!' || *p == '^');
  if (is_not) ++p;

  c = *p++;
  for (;;) {
    cstart = cend = c;
    forcecoll = 0;

    if (c == '[' && *p == '=' && (close = parse_subbracket(p, flags)) != nullptr) {
      p++;
      pc = fold(collsym(p, static_cast<int>(close - p)), flags);
      p = close + 2;
      if (test == static_cast<UC>(pc)) {
        p++;
        goto matched;
      }
      c = *p++;
      if (c == '\0') return (test == '[') ? savep : nullptr;
      if (c == '/' && (flags & FNM_PATHNAME)) return (test == '[') ? savep : nullptr;
      if (c == ']') break;
      c = static_cast<UC>(fold(c, flags));
      continue;
    }

    if (c == '[' && *p == ':' && (close = parse_subbracket(p, flags)) != nullptr) {
      int len = static_cast<int>(close - p - 1);
      char *ccname = static_cast<char *>(std::malloc(static_cast<size_t>(len + 1)));
      pc = 0;
      if (ccname) {
        std::memcpy(ccname, p + 1, static_cast<size_t>(len));
        ccname[len] = '\0';
        dequote_pathname(reinterpret_cast<UC *>(ccname));
        pc = is_cclass(test_cp, ccname);
        if (pc == -1) pc = 0;
        std::free(ccname);
      }
      p = close + 2;
      if (pc) {
        p++;
        goto matched;
      }
      c = *p++;
      if (c == '\0') return (test == '[') ? savep : nullptr;
      if (c == '/' && (flags & FNM_PATHNAME)) return (test == '[') ? savep : nullptr;
      if (c == ']') break;
      c = static_cast<UC>(fold(c, flags));
      continue;
    }

    if (c == '[' && *p == '.' && (close = parse_subbracket(p, flags)) != nullptr) {
      p++;
      cstart = static_cast<UC>(collsym(p, static_cast<int>(close - p)));
      p = close + 2;
      forcecoll = 1;
    }

    if (!(flags & FNM_NOESCAPE) && c == '\\') {
      if (*p == '\0') return (test == '[') ? savep : nullptr;
      if (*p == '/' && (flags & FNM_PATHNAME)) return (test == '[') ? savep : nullptr;
      cstart = cend = *p++;
    }

    cstart = cend = static_cast<UC>(fold(cstart, flags));
    isrange = 0;

    if (c == '\0') return (test == '[') ? savep : nullptr;
    if (c == '/' && (flags & FNM_PATHNAME)) return (test == '[') ? savep : nullptr;

    c = static_cast<UC>(fold(*p++, flags));
    if (c == '\0') return (test == '[') ? savep : nullptr;
    if (c == '/' && (flags & FNM_PATHNAME)) return (test == '[') ? savep : nullptr;

    if (c == '-' && *p != ']') {
      cend = *p++;
      if (!(flags & FNM_NOESCAPE) && cend == '\\') cend = *p++;
      if (cend == '\0') return (test == '[') ? savep : nullptr;
      if (cend == '/' && (flags & FNM_PATHNAME)) return (test == '[') ? savep : nullptr;
      if (cend == '[' && *p == '.' && (close = parse_subbracket(p, flags)) != nullptr) {
        p++;
        cend = static_cast<UC>(collsym(p, static_cast<int>(close - p)));
        p = close + 2;
        forcecoll = 1;
      }
      cend = static_cast<UC>(fold(cend, flags));
      c = *p++;
      (void)forcecoll;
      if (rangecmp(cstart, cend) > 0) {
        if (c == ']') break;
        c = static_cast<UC>(fold(c, flags));
        continue;
      }
      isrange = 1;
    }

    if (isrange == 0 && test == cstart) goto matched;
    if (isrange && rangecmp(test, cstart) >= 0 && rangecmp(test, cend) <= 0) goto matched;

    if (c == ']') break;
  }
  /* Scanned the whole class with no match: success iff the class was negated. */
  return is_not ? p : nullptr;

matched:
  c = *--p;
  while (1) {
    if (c == '\0') return (test == '[') ? savep : nullptr;
    if (c == '/' && (flags & FNM_PATHNAME)) return (test == '[') ? savep : nullptr;
    c = *p++;
    if (c == '[' && (*p == '=' || *p == ':' || *p == '.')) {
      if ((close = parse_subbracket(p, flags)) != nullptr) p = close + 2;
    } else if (c == ']') {
      break;
    } else if (!(flags & FNM_NOESCAPE) && c == '\\') {
      if (*p == '\0') return (test == '[') ? savep : nullptr;
      if (*p == '/' && (flags & FNM_PATHNAME)) return (test == '[') ? savep : nullptr;
      ++p;
    }
  }
  return is_not ? nullptr : p;
}

UC *patscan(UC *string, UC *end, int delim, int flags) {
  int pnest = 0, bnest = 0, skip = 0;
  UC *s, c, *bfirst = nullptr, *t;

  if (string == end) return nullptr;

  for (s = string; (c = *s); s++) {
    if (s >= end) return s;
    if (skip) {
      skip = 0;
      continue;
    }
    switch (c) {
      case '\\':
        if ((flags & FNM_NOESCAPE) == 0) skip = 1;
        break;
      case '\0':
        return nullptr;
      case '[':
        if (bnest == 0) {
          bfirst = s + 1;
          if (*bfirst == '!' || *bfirst == '^') bfirst++;
          bnest++;
        } else if (s[1] == ':' || s[1] == '.' || s[1] == '=') {
          t = parse_subbracket(s + 1, flags);
          if (t) s = t + 2 - 1;
        }
        break;
      case ']':
        if (bnest) {
          if (s != bfirst) {
            bnest--;
            bfirst = nullptr;
          }
        }
        break;
      case '(':
        if (bnest == 0) pnest++;
        break;
      case ')':
        if (bnest == 0 && pnest-- <= 0) return ++s;
        break;
      case '|':
        if (bnest == 0 && pnest == 0 && delim == '|') return ++s;
        break;
    }
  }
  return nullptr;
}

int strcompare(UC *p, UC *pe, UC *s, UC *se) {
  ptrdiff_t l1 = pe - p, l2 = se - s;
  if (l1 != l2) return FNM_NOMATCH;
  return (std::memcmp(p, s, static_cast<size_t>(l1)) == 0) ? 0 : FNM_NOMATCH;
}

int extmatch(int xc, UC *s, UC *se, UC *p, UC *pe, int flags) {
  UC *prest, *psub, *pnext, *srest;
  int m1, m2 = 0, xflags;

  prest = patscan(p + (*p == '('), pe, 0, flags);
  if (prest == nullptr) return strcompare(p - 1, pe, s, se);

  switch (xc) {
    case '+':
    case '*':
      if (xc == '*' && gmatch(s, se, prest, pe, nullptr, flags) == 0) return 0;
      for (psub = p + 1;; psub = pnext) {
        pnext = patscan(psub, pe, '|', flags);
        for (srest = s; srest <= se; srest++) {
          m1 = gmatch(s, srest, psub, pnext - 1, nullptr, flags) == 0;
          if (m1) {
            xflags = (srest > s) ? (flags & ~(FNM_PERIOD | FNM_DOTDOT)) : flags;
            m2 = (gmatch(srest, se, prest, pe, nullptr, xflags) == 0) ||
                 (s != srest && gmatch(srest, se, p - 1, pe, nullptr, xflags) == 0);
          }
          if (m1 && m2) return 0;
        }
        if (pnext == prest) break;
      }
      return FNM_NOMATCH;

    case '?':
    case '@':
      if (xc == '?' && gmatch(s, se, prest, pe, nullptr, flags) == 0) return 0;
      for (psub = p + 1;; psub = pnext) {
        pnext = patscan(psub, pe, '|', flags);
        srest = (prest == pe) ? se : s;
        for (; srest <= se; srest++) {
          xflags = (srest > s) ? (flags & ~(FNM_PERIOD | FNM_DOTDOT)) : flags;
          if (gmatch(s, srest, psub, pnext - 1, nullptr, flags) == 0 &&
              gmatch(srest, se, prest, pe, nullptr, xflags) == 0)
            return 0;
        }
        if (pnext == prest) break;
      }
      return FNM_NOMATCH;

    case '!':
      for (srest = s; srest <= se; srest++) {
        m1 = 0;
        for (psub = p + 1;; psub = pnext) {
          pnext = patscan(psub, pe, '|', flags);
          if ((m1 = (gmatch(s, srest, psub, pnext - 1, nullptr, flags) == 0))) break;
          if (pnext == prest) break;
        }
        if (m1 == 0 && (flags & FNM_PERIOD) && *s == '.') return FNM_NOMATCH;
        if (m1 == 0 && (flags & FNM_DOTDOT) && sdot_or_dotdot(s)) return FNM_NOMATCH;
        xflags = (srest > s) ? (flags & ~(FNM_PERIOD | FNM_DOTDOT)) : flags;
        if (m1 == 0 && gmatch(srest, se, prest, pe, nullptr, xflags) == 0) return 0;
      }
      return FNM_NOMATCH;
  }
  return FNM_NOMATCH;
}

int gmatch(UC *string, UC *se, UC *pattern, UC *pe, SmEnds *ends, int flags) {
  UC *p, *n;
  int c, sc;
  size_t nadv;  // bytes to advance over the matched string character

  if (string == nullptr || pattern == nullptr) return FNM_NOMATCH;
  p = pattern;
  n = string;

  while (p < pe) {
    c = *p++;
    c = fold(c, flags);
    sc = n < se ? *n : '\0';
    nadv = 1;  // a `?'/`['/multibyte literal steps over a whole character below

    if (interrupt_state || terminating_signal) return FNM_NOMATCH;

    if ((flags & FNM_EXTMATCH) && *p == '(' &&
        (c == '+' || c == '*' || c == '?' || c == '@' || c == '!')) {
      int lflags = (n == string) ? flags : (flags & ~(FNM_PERIOD | FNM_DOTDOT));
      return extmatch(c, n, se, p, pe, lflags);
    }

    switch (c) {
      case '?':
        if (sc == '\0') return FNM_NOMATCH;
        if ((flags & FNM_PATHNAME) && sc == '/') return FNM_NOMATCH;
        if ((flags & FNM_PERIOD) && sc == '.' &&
            (n == string || ((flags & FNM_PATHNAME) && n[-1] == '/')))
          return FNM_NOMATCH;
        if ((flags & FNM_DOTDOT) &&
            ((n == string && sdot_or_dotdot(n)) ||
             ((flags & FNM_PATHNAME) && n > string && n[-1] == '/' && sdot_or_dotdot(n))))
          return FNM_NOMATCH;
        nadv = mb_clen(n, se);  // `?' matches one whole character
        break;

      case '\\':
        if (p == pe && sc == '\\' && (n + 1 == se)) break;
        if (p == pe) return FNM_NOMATCH;
        if ((flags & FNM_NOESCAPE) == 0) {
          c = *p++;
          if (p > pe) return FNM_NOMATCH;
          c = fold(c, flags);
        }
        if (fold(sc, flags) != c) return FNM_NOMATCH;
        break;

      case '*': {
        if (ends != nullptr) {
          ends->pattern = p - 1;
          ends->string = n;
          return 0;
        }
        if ((flags & FNM_PERIOD) && sc == '.' &&
            (n == string || ((flags & FNM_PATHNAME) && n[-1] == '/')))
          return FNM_NOMATCH;
        if ((flags & FNM_DOTDOT) &&
            ((n == string && sdot_or_dotdot(n)) ||
             ((flags & FNM_PATHNAME) && n > string && n[-1] == '/' && sdot_or_dotdot(n))))
          return FNM_NOMATCH;
        if (p == pe) return 0;

        for (c = *p++; (c == '?' || c == '*'); c = *p++) {
          if ((flags & FNM_PATHNAME) && sc == '/') return FNM_NOMATCH;
          if ((flags & FNM_EXTMATCH) && c == '?' && *p == '(') {
            UC *newn;
            if (extmatch(c, n, se, p, pe, flags) == 0) return 0;
            newn = patscan(p + 1, pe, 0, flags);
            p = newn ? newn : pe;
          } else if (c == '?') {
            if (sc == '\0') return FNM_NOMATCH;
            n += mb_clen(n, se);  // `?' consumes one whole character
            sc = n < se ? *n : '\0';
          }
          if ((flags & FNM_EXTMATCH) && c == '*' && *p == '(') {
            UC *newn;
            for (newn = n; newn < se; ++newn)
              if (extmatch(c, newn, se, p, pe, flags) == 0) return 0;
            newn = patscan(p + 1, pe, 0, flags);
            p = newn ? newn : pe;
          }
          if (p == pe) break;
        }

        if (c == '\0') {
          int r = (flags & FNM_PATHNAME) == 0 ? 0 : FNM_NOMATCH;
          if (flags & FNM_PATHNAME) {
            if (flags & FNM_LEADING_DIR)
              r = 0;
            else if (std::memchr(n, '/', static_cast<size_t>(se - n)) == nullptr)
              r = 0;
          }
          return r;
        }
        if (p == pe && (c == '?' || c == '*')) return 0;

        if (n == se &&
            ((flags & FNM_EXTMATCH) && (c == '!' || c == '?') && *p == '(')) {
          --p;
          if (extmatch(c, n, se, p, pe, flags) == 0) return (c == '!') ? FNM_NOMATCH : 0;
          return (c == '!') ? 0 : FNM_NOMATCH;
        }

        if (c == '/' && (flags & FNM_PATHNAME)) {
          while (n < se && *n != '/') ++n;
          if (n < se && *n == '/' && gmatch(n + 1, se, p, pe, nullptr, flags) == 0) return 0;
          return FNM_NOMATCH;
        }

        {
          UC c1;
          const UC *endp;
          SmEnds end;
          end.pattern = nullptr;
          endp = static_cast<const UC *>(
              std::memchr(n, (flags & FNM_PATHNAME) ? '/' : '\0', static_cast<size_t>(se - n)));
          if (endp == nullptr) endp = se;
          c1 = static_cast<UC>(((flags & FNM_NOESCAPE) == 0 && c == '\\') ? *p : c);
          c1 = static_cast<UC>(fold(c1, flags));
          for (--p; n < endp; ++n) {
            if ((flags & FNM_EXTMATCH) == 0 && c != '[' &&
                static_cast<UC>(fold(*n, flags)) != c1)
              continue;
            if ((flags & FNM_EXTMATCH) && p[1] != '(' &&
                std::strchr("?*+@!", *p) == nullptr && c != '[' &&
                static_cast<UC>(fold(*n, flags)) != c1)
              continue;
            if (gmatch(n, se, p, pe, &end, flags & ~(FNM_PERIOD | FNM_DOTDOT)) == 0) {
              if (end.pattern == nullptr) return 0;
              break;
            }
          }
          if (end.pattern != nullptr) {
            p = end.pattern;
            n = end.string;
            continue;
          }
          return FNM_NOMATCH;
        }
      }

      case '[': {
        if (sc == '\0' || n == se) return FNM_NOMATCH;
        if ((flags & FNM_PERIOD) && sc == '.' &&
            (n == string || ((flags & FNM_PATHNAME) && n[-1] == '/')))
          return FNM_NOMATCH;
        if (sc == '/' && (flags & FNM_PATHNAME)) return FNM_NOMATCH;
        if ((flags & FNM_DOTDOT) &&
            ((n == string && sdot_or_dotdot(n)) ||
             ((flags & FNM_PATHNAME) && n > string && n[-1] == '/' && sdot_or_dotdot(n))))
          return FNM_NOMATCH;
        {
          size_t clen = 1;
          std::wint_t cp = mb_cp(n, se, clen);
          p = brackmatch(p, static_cast<UC>(sc), cp, flags);
          if (p == nullptr) return FNM_NOMATCH;
          nadv = clen;  // the bracket matched one whole character
        }
        break;
      }

      default:
        if (static_cast<UC>(c) != fold(sc, flags)) return FNM_NOMATCH;
    }
    n += nadv;
  }

  if (n == se) return 0;
  if ((flags & FNM_LEADING_DIR) && *n == '/') return 0;
  return FNM_NOMATCH;
}

}  // namespace

extern "C" int strmatch(char *pattern, char *string, int flags) {
  if (string == nullptr || pattern == nullptr) return FNM_NOMATCH;
  UC *s = reinterpret_cast<UC *>(string);
  UC *p = reinterpret_cast<UC *>(pattern);
  UC *se = s + std::strlen(string);
  UC *pe = p + std::strlen(pattern);
  return gmatch(s, se, p, pe, nullptr, flags);
}
