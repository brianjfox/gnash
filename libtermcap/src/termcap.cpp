// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// termcap.cpp -- a termcap work-alike.
//
// The capability-parsing engine (find_capability, tgetst1, tgetnum, tgetflag,
// tgetstr) and the output/parameter routines (tputs, tparam, tgoto) are ported
// verbatim from bash 5.3 lib/termcap.  tgetent() is reimplemented to source
// entries from memory: an optional $TERMCAP entry, an optional termcap file,
// and the compiled-in built-in database (termcap_db.cpp).  This keeps the
// observable tget*/tputs/tgoto semantics identical while working on systems
// (macOS, ...) that have no /etc/termcap.

#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "gnash/sh/xmalloc.hpp"
#include "termcap.h"

using gnash::sh::xmalloc;
using gnash::sh::xrealloc;

namespace gnash::termcap {
extern const char *builtin_database;
}

// ---------------------------------------------------------------------------
// The entry found by tgetent, consulted by tget{num,flag,str}.
// ---------------------------------------------------------------------------
static std::string g_entry;
static const char *term_entry;

// Search entry BP for capability CAP; return a pointer just past ":xx" or NULL.
static const char *find_capability(const char *bp, const char *cap) {
  for (; *bp; bp++)
    if (bp[0] == ':' && bp[1] == cap[0] && bp[2] == cap[1]) return &bp[4];
  return nullptr;
}

extern "C" int tgetnum(const char *cap) {
  const char *ptr = find_capability(term_entry, cap);
  if (!ptr || ptr[-1] != '#') return -1;
  return std::atoi(ptr);
}

extern "C" int tgetflag(const char *cap) {
  const char *ptr = find_capability(term_entry, cap);
  return (ptr && ptr[-1] == ':') ? 1 : 0;
}

// Character-abbreviation table for tgetst1, indexed by (c & ~040) - 0100.
static char esctab[] =
    " \007\010  \033\014 \
      \012 \
  \015 \011 \013 \
        ";

// Copy a string-valued capability, processing ^ and \ escapes.
static char *tgetst1(const char *ptr, char **area) {
  const char *p;
  char *r, *ret;
  int c, size, c1;

  if (!ptr) return nullptr;

  if (!area) {
    p = ptr;
    while ((c = *p++) && c != ':' && c != '\n')
      ;
    ret = static_cast<char *>(xmalloc(static_cast<size_t>(p - ptr + 1)));
  } else {
    ret = *area;
  }

  p = ptr;
  r = ret;
  while ((c = *p++) && c != ':' && c != '\n') {
    if (c == '^') {
      c = *p++;
      if (c == '?')
        c = 0177;
      else
        c &= 037;
    } else if (c == '\\') {
      c = *p++;
      if (c >= '0' && c <= '7') {
        c -= '0';
        size = 0;
        while (++size < 3 && (c1 = *p) >= '0' && c1 <= '7') {
          c *= 8;
          c += c1 - '0';
          p++;
        }
      } else if (c >= 0100 && c < 0200) {
        c1 = esctab[(c & ~040) - 0100];
        if (c1 != ' ') c = c1;
      }
    }
    *r++ = static_cast<char>(c);
  }
  *r = '\0';
  if (area) *area = r + 1;
  return ret;
}

extern "C" char *tgetstr(const char *cap, char **area) {
  const char *ptr = find_capability(term_entry, cap);
  if (!ptr || (ptr[-1] != '=' && ptr[-1] != '~')) return nullptr;
  return tgetst1(ptr, area);
}

// ---------------------------------------------------------------------------
// Padding output.
// ---------------------------------------------------------------------------
short ospeed;
int tputs_baud_rate;
char PC = '\0';

static int speeds[] = {0,  50,  75,  110, 135,  150,  -2,   -3,  -6,  -12,
                       -18, -24, -48, -96, -192, -288, -384, -576, -1152};

extern "C" int tputs(const char *str, int nlines, int (*outfun)(int)) {
  int padcount = 0;
  int speed;

  if (ospeed == 0)
    speed = tputs_baud_rate;
  else if (ospeed > 0 && ospeed < static_cast<int>(sizeof speeds / sizeof speeds[0]))
    speed = speeds[ospeed];
  else
    speed = 0;

  if (!str) return -1;

  while (*str >= '0' && *str <= '9') {
    padcount += *str++ - '0';
    padcount *= 10;
  }
  if (*str == '.') {
    str++;
    padcount += *str++ - '0';
  }
  if (*str == '*') {
    str++;
    padcount *= nlines;
  }
  while (*str) (*outfun)(*str++);

  padcount *= speed;
  padcount += 500;
  padcount /= 1000;
  if (speed < 0)
    padcount = -padcount;
  else {
    padcount += 50;
    padcount /= 100;
  }

  while (padcount-- > 0) (*outfun)(PC);
  return 0;
}

// ---------------------------------------------------------------------------
// Parameter substitution: tparam / tgoto (ported from tparam.c).
// ---------------------------------------------------------------------------
char *BC;
char *UP;
static char tgoto_buf[50];

static char *tparam1(const char *string, char *outstring, int len, char *up,
                     char *left, int *argp) {
  int c;
  const char *p = string;
  char *op = outstring;
  char *outend;
  int outlen = 0;
  int tem;
  int *old_argp = argp;
  int doleft = 0;
  int doup = 0;

  outend = outstring + len;

  while (1) {
    if (op + 5 >= outend) {
      char *nw;
      int offset = static_cast<int>(op - outstring);
      if (outlen == 0) {
        outlen = len + 40;
        nw = static_cast<char *>(xmalloc(static_cast<size_t>(outlen)));
        std::memcpy(nw, outstring, static_cast<size_t>(offset));
      } else {
        outlen *= 2;
        nw = static_cast<char *>(xrealloc(outstring, static_cast<size_t>(outlen)));
      }
      op = nw + offset;
      outend = nw + outlen;
      outstring = nw;
    }
    c = *p++;
    if (!c) break;
    if (c == '%') {
      c = *p++;
      tem = *argp;
      switch (c) {
        case 'd':
          if (tem < 10) goto onedigit;
          if (tem < 100) goto twodigit;
          [[fallthrough]];
        case '3':
          if (tem > 999) {
            *op++ = static_cast<char>(tem / 1000 + '0');
            tem %= 1000;
          }
          *op++ = static_cast<char>(tem / 100 + '0');
          [[fallthrough]];
        case '2':
        twodigit:
          tem %= 100;
          *op++ = static_cast<char>(tem / 10 + '0');
        onedigit:
          *op++ = static_cast<char>(tem % 10 + '0');
          argp++;
          break;

        case 'C':
          if (tem >= 96) {
            *op++ = static_cast<char>(tem / 96);
            tem %= 96;
          }
          [[fallthrough]];
        case '+':
          tem += *p++;
          [[fallthrough]];
        case '.':
          if (left) {
            while (tem == 0 || tem == '\n' || tem == '\t') {
              tem++;
              if (argp == old_argp)
                doup++, outend -= std::strlen(up);
              else
                doleft++, outend -= std::strlen(left);
            }
          }
          *op++ = static_cast<char>(tem ? tem : 0200);
          [[fallthrough]];
        case 'f':
          argp++;
          break;

        case 'b':
          argp--;
          break;

        case 'r':
          argp[0] = argp[1];
          argp[1] = tem;
          old_argp++;
          break;

        case '>':
          if (argp[0] > *p++) argp[0] += *p;
          p++;
          break;

        case 'a':
          tem = p[2] & 0177;
          if (p[1] == 'p') tem = argp[tem - 0100];
          if (p[0] == '-')
            argp[0] -= tem;
          else if (p[0] == '+')
            argp[0] += tem;
          else if (p[0] == '*')
            argp[0] *= tem;
          else if (p[0] == '/')
            argp[0] /= tem;
          else
            argp[0] = tem;
          p += 3;
          break;

        case 'i':
          argp[0]++;
          argp[1]++;
          break;

        case '%':
          goto ordinary;

        case 'n':
          argp[0] ^= 0140;
          argp[1] ^= 0140;
          break;

        case 'm':
          argp[0] ^= 0177;
          argp[1] ^= 0177;
          break;

        case 'B':
          argp[0] += 6 * (tem / 10);
          break;

        case 'D':
          argp[0] -= 2 * (tem % 16);
          break;
      }
    } else
    ordinary:
      *op++ = static_cast<char>(c);
  }
  *op = 0;
  while (doup-- > 0) std::strcat(op, up);
  while (doleft-- > 0) std::strcat(op, left);
  return outstring;
}

extern "C" char *tparam(const char *string, char *outstring, int len, ...) {
  // bash's tparam takes up to four integer parameters.
  int arg[4] = {0, 0, 0, 0};
  va_list ap;
  va_start(ap, len);
  for (int i = 0; i < 4; i++) arg[i] = va_arg(ap, int);
  va_end(ap);
  return tparam1(string, outstring, len, nullptr, nullptr, arg);
}

extern "C" char *tgoto(const char *cm, int hpos, int vpos) {
  int args[2];
  if (!cm) return nullptr;
  args[0] = vpos;
  args[1] = hpos;
  return tparam1(cm, tgoto_buf, 50, UP, BC, args);
}

// ---------------------------------------------------------------------------
// tgetent: locate the entry for NAME from the in-memory corpus.
// ---------------------------------------------------------------------------
namespace {

// Unfold a termcap corpus into logical entries (joining `\`-continued lines,
// skipping blank lines and comments).
std::vector<std::string> split_entries(const std::string &corpus) {
  std::vector<std::string> entries;
  std::string cur;
  std::size_t i = 0;
  while (i <= corpus.size()) {
    std::size_t nl = corpus.find('\n', i);
    std::string line =
        (nl == std::string::npos) ? corpus.substr(i) : corpus.substr(i, nl - i);
    if (!line.empty() && line.back() == '\r') line.pop_back();

    bool last = (nl == std::string::npos);
    if (cur.empty() && (line.empty() || line[0] == '#')) {
      if (last) break;
      i = nl + 1;
      continue;
    }
    if (!line.empty() && line.back() == '\\') {
      cur += line.substr(0, line.size() - 1);  // continued
    } else {
      cur += line;
      if (!cur.empty()) entries.push_back(cur);
      cur.clear();
    }
    if (last) break;
    i = nl + 1;
  }
  if (!cur.empty()) entries.push_back(cur);
  return entries;
}

// Does logical entry E name TERM (in its `|`-separated name field)?
bool entry_names(const std::string &e, const char *name) {
  std::size_t colon = e.find(':');
  std::string names = (colon == std::string::npos) ? e : e.substr(0, colon);
  std::size_t start = 0;
  while (start <= names.size()) {
    std::size_t bar = names.find('|', start);
    std::string one = (bar == std::string::npos) ? names.substr(start)
                                                 : names.substr(start, bar - start);
    if (one == name) return true;
    if (bar == std::string::npos) break;
    start = bar + 1;
  }
  return false;
}

// Resolve NAME within CORPUS, following one level of `tc=` indirection, and
// append the resolved chain (specific entry first so it wins in lookups).
bool resolve(const std::vector<std::string> &entries, const char *name,
             std::string &out, std::vector<std::string> &visited) {
  for (const std::string &e : entries) {
    if (!entry_names(e, name)) continue;
    out += e;
    const char *tc = find_capability(e.c_str(), "tc");
    if (tc && (tc[-1] == '=' || tc[-1] == '~')) {
      char *ref = tgetst1(tc, nullptr);
      if (ref) {
        bool seen = false;
        for (const std::string &v : visited)
          if (v == ref) seen = true;
        if (!seen) {
          visited.emplace_back(ref);
          out += ':';
          resolve(entries, ref, out, visited);
        }
        gnash::sh::xfree(ref);
      }
    }
    return true;
  }
  return false;
}

std::string read_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::string();
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

extern "C" int tgetent(char *bp, const char *name) {
  std::string corpus;

  // 1. $TERMCAP: either an entry (used verbatim) or a file path.
  const char *tc = std::getenv("TERMCAP");
  std::string tcfile;
  if (tc && *tc) {
    if (tc[0] == '/')
      tcfile = tc;  // a file to read instead of /etc/termcap
    else
      corpus += std::string(tc) + "\n";  // an inline entry (highest priority)
  }

  // 2. A termcap file, if one is available.
  std::string file = read_file(tcfile.empty() ? "/etc/termcap" : tcfile);
  if (!file.empty()) corpus += file + "\n";

  // 3. The compiled-in database.
  corpus += gnash::termcap::builtin_database;

  std::vector<std::string> entries = split_entries(corpus);
  std::string found;
  std::vector<std::string> visited;
  visited.emplace_back(name);
  if (!resolve(entries, name, found, visited)) {
    g_entry.clear();
    term_entry = g_entry.c_str();
    return 0;  // database accessible, entry not defined
  }

  g_entry = found;
  if (bp) {
    std::strcpy(bp, g_entry.c_str());
    term_entry = bp;
  } else {
    term_entry = g_entry.c_str();
  }
  return 1;
}
