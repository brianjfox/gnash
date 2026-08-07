// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// expand.cpp -- word expansion (see expand.hpp).
//
// A pragmatic but faithful implementation of the bash expansion pipeline.  Not
// yet covered: arrays (${a[@]}), ${!prefix*}, some locale/case operators; these
// are follow-ons.

#include "gnash/core/builtins.hpp"
#include "gnash/core/expand.hpp"
#include "gnash/core/lexer.hpp"
#include "gnash/core/subscript.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <functional>
#ifdef GNASH_HAVE_ICONV
#include <iconv.h>
#endif
#include <langinfo.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>

#include "gnash/glob.hpp"
#include "glob.h"
#include "strmatch.h"

namespace gnash::core {

namespace {

constexpr char FIELD_SEP = '\x01';  // internal "$@" field boundary marker
// Quoted-null marker: emitted when a quote region opens, so an expansion that
// yields empty text still leaves a field ("" / "$empty" -> one empty field).
// A double-quoted "$@"/"${a[@]}" that expands to nothing absorbs the marker,
// so it yields *no* field (matching bash).  Stripped before the result is used.
constexpr char QNULL = '\x02';
// Mask letter for the marker bytes themselves: only a FIELD_SEP/QNULL whose
// mask is MMARK is an internal marker; the same byte under any other mask is
// literal data (e.g. $'\001').
constexpr char MMARK = 'M';

bool pat_match(const std::string &pattern, const std::string &text) {
  std::string p = pattern, t = text;
  return strmatch(p.data(), t.data(), FNM_EXTMATCH) == 0;
}

// Scan a balanced span from `open`/`close` starting with text[i] at the opener;
// returns the index of the matching closer (or npos).  Honors quotes.  With
// `firstclose` (used for ${param...}), a bare `{` does not open a nested level
// -- only a `${` does -- matching bash's parse_matched_pair P_FIRSTCLOSE rule,
// so `${IFS+a{b}` closes at the `}` after `b`.
size_t scan_balanced(const std::string &t, size_t i, char open, char close,
                     bool firstclose = false, bool squote_literal = false) {
  int depth = 0;
  bool wasdol = false;  // previous char was an unquoted `$` (LEX_WASDOL)
  // When scanning a `$( ... )' command substitution, a `)' that terminates a
  // `case' pattern (`case x in x)') is not the closer.  Track the depth of each
  // command-position `case' body (mirrors the lexer's scan_paren).  Inactive for
  // `[' / `{' scans, which are byte-identical to before.
  const bool case_aware = (open == '(');
  bool cmd_pos = true;
  std::vector<int> case_stack;
  std::vector<std::pair<std::string, bool>> heredocs;  // pending <<delim, <<-
  std::string word;
  bool word_plain = true;
  bool saw_word = false;
  auto boundary = [&]() {
    if (case_aware && saw_word) {
      if (cmd_pos && word_plain && word == "case") case_stack.push_back(depth);
      else if (cmd_pos && word_plain && word == "esac" && !case_stack.empty())
        case_stack.pop_back();
      cmd_pos = word_plain && (word == "then" || word == "do" ||
                               word == "else" || word == "elif");
    }
    word.clear();
    word_plain = true;
    saw_word = false;
  };
  auto contaminate = [&]() { if (case_aware) { saw_word = true; word_plain = false; } };
  for (; i < t.size(); i++) {
    char c = t[i];
    if (case_aware && c == '<' && i + 1 < t.size() && t[i + 1] == '<' &&
        i + 2 < t.size() && t[i + 2] == '<') {
      i += 2;  // `<<<' here-string: skip the operator whole (loop adds 1)
    } else if (case_aware && c == '<' && i + 1 < t.size() && t[i + 1] == '<' &&
        !(i + 2 < t.size() && t[i + 2] == '<')) {
      // Remember a here-document delimiter; its body lines are skipped
      // verbatim at the next newline (a `)' in them is not the closer).
      i += 2;
      bool strip_tabs = i < t.size() && t[i] == '-';
      if (strip_tabs) i++;
      while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) i++;
      std::string delim;
      while (i < t.size() && !std::isspace(static_cast<unsigned char>(t[i])) &&
             !std::strchr(";&|()<>", t[i])) {
        char dc = t[i];
        if (dc == '\'' || dc == '"') {
          char q = dc;
          i++;
          while (i < t.size() && t[i] != q) delim += t[i++];
          if (i < t.size()) i++;
          continue;
        }
        if (dc == '\\' && i + 1 < t.size()) { i++; dc = t[i]; }
        delim += dc;
        i++;
      }
      i--;  // loop increment
      if (!delim.empty()) heredocs.push_back({delim, strip_tabs});
      contaminate();
      wasdol = false;
      continue;
    }
    if (case_aware && c == '\n' && !heredocs.empty()) {
      i++;  // past the newline
      bool closing = false;
      for (auto &hd : heredocs) {
        if (closing) break;
        while (i < t.size()) {
          size_t ls = i;
          while (i < t.size() && t[i] != '\n') i++;
          std::string line = t.substr(ls, i - ls);
          bool had_nl = i < t.size();
          std::string cmp = line;
          size_t tt = 0;
          if (hd.second)
            while (tt < cmp.size() && cmp[tt] == '\t') tt++;
          cmp = cmp.substr(tt);
          // `EOF)': the closer may abut the delimiter -- resume at the `)'.
          if (cmp.compare(0, hd.first.size(), hd.first) == 0 &&
              cmp.size() > hd.first.size() && cmp[hd.first.size()] == ')') {
            i = ls + tt + hd.first.size();
            closing = true;
            break;
          }
          if (had_nl) i++;
          if (cmp == hd.first || !had_nl) break;
        }
      }
      heredocs.clear();
      i--;  // loop increment
      if (case_aware) boundary();
      cmd_pos = true;
      wasdol = false;
      continue;
    }
    if (c == '\\') { contaminate(); i++; wasdol = false; continue; }
    if (c == '$' && i + 1 < t.size() && t[i + 1] == '\'') {
      // $'...': ANSI-C quoting, where a backslash escapes the next character
      // (so \' is not a terminator).
      contaminate();
      i += 2;
      while (i < t.size() && t[i] != '\'') { if (t[i] == '\\' && i + 1 < t.size()) i++; i++; }
      wasdol = false;
      continue;
    }
    if (c == '\'') {
      // POSIX mode, inside a double-quoted ${...}: a single quote is an
      // ordinary character (`"${IFS+'}'z}"` closes at the first `}`).
      if (squote_literal) { wasdol = false; continue; }
      contaminate(); while (++i < t.size() && t[i] != '\'') {} wasdol = false; continue;
    }
    if (c == '"') {
      contaminate();
      while (++i < t.size() && t[i] != '"')
        if (t[i] == '\\') i++;
      wasdol = false;
      continue;
    }
    // A nested `$( ... )' / `$(( ... ))' is skipped by paren balancing so its
    // inner `{'/`}' (e.g. brace expansion `$(echo a{b,c})') do not disturb this
    // scan's `{' depth -- unless we are ourselves scanning a `(...)' group.
    if (c == '$' && open != '(' && i + 1 < t.size() && t[i + 1] == '(') {
      size_t inner = scan_balanced(t, i + 1, '(', ')');
      if (inner == std::string::npos) return std::string::npos;
      i = inner;  // now at the matching `)'
      wasdol = false;
      continue;
    }
    if (c == open) {
      if (case_aware) boundary();
      if (!firstclose || open != '{' || depth == 0 || wasdol) depth++;
      if (case_aware) cmd_pos = true;
    } else if (c == close) {
      if (case_aware) {
        boundary();
        if (!case_stack.empty() && case_stack.back() == depth) {
          cmd_pos = true;  // case pattern terminator: keep depth, stay open
          wasdol = false;
          continue;
        }
      }
      if (--depth == 0) return i;
      if (case_aware) cmd_pos = true;
    } else if (case_aware && (c == ';' || c == '&' || c == '|' || c == '\n')) {
      boundary();
      cmd_pos = true;
    } else if (case_aware && (c == ' ' || c == '\t')) {
      boundary();
    } else if (case_aware && c == '#' && !saw_word) {
      // A comment runs to the end of the line: a `)' in it is not the closer.
      while (i + 1 < t.size() && t[i + 1] != '\n') i++;
    } else if (case_aware && c == '$') {
      saw_word = true;
      word_plain = false;
    } else if (case_aware) {
      saw_word = true;
      if (word_plain && (std::isalnum(static_cast<unsigned char>(c)) || c == '_')) word += c;
      else word_plain = false;
    }
    wasdol = (c == '$');
  }
  return std::string::npos;
}

// --- multibyte helpers ------------------------------------------------------
// Character (code point) length of s under the current LC_CTYPE.  In a unibyte
// or C locale (MB_CUR_MAX==1) this is the byte count, exactly as bash falls
// back.  A malformed or truncated sequence counts as one character (and resets
// the shift state), matching bash's tolerance for invalid input.
size_t mb_charlen(const std::string &s) {
  if (MB_CUR_MAX <= 1) return s.size();
  size_t n = 0, i = 0;
  std::mbstate_t st{};
  while (i < s.size()) {
    size_t r = std::mbrtowc(nullptr, s.data() + i, s.size() - i, &st);
    if (r == static_cast<size_t>(-1) || r == static_cast<size_t>(-2) || r == 0) {
      r = 1;
      st = std::mbstate_t{};
    }
    i += r;
    n++;
  }
  return n;
}

// Byte offset of character index c within s (clamped to s.size()), the inverse
// of mb_charlen used to turn a character offset/length into a byte slice.
size_t mb_byteoff(const std::string &s, size_t c) {
  if (MB_CUR_MAX <= 1) return c < s.size() ? c : s.size();
  size_t i = 0, k = 0;
  std::mbstate_t st{};
  while (i < s.size() && k < c) {
    size_t r = std::mbrtowc(nullptr, s.data() + i, s.size() - i, &st);
    if (r == static_cast<size_t>(-1) || r == static_cast<size_t>(-2) || r == 0) {
      r = 1;
      st = std::mbstate_t{};
    }
    i += r;
    k++;
  }
  return i;
}

// Decode the character starting at s[i]: returns its wide value and sets len to
// its byte length (>=1).  On an invalid/truncated sequence (or a unibyte
// locale) it yields the raw byte with len 1, so callers step forward safely.
static wchar_t mb_decode(const std::string &s, size_t i, size_t &len) {
  if (MB_CUR_MAX <= 1) { len = 1; return static_cast<unsigned char>(s[i]); }
  wchar_t wc = 0;
  std::mbstate_t st{};
  size_t r = std::mbrtowc(&wc, s.data() + i, s.size() - i, &st);
  if (r == static_cast<size_t>(-1) || r == static_cast<size_t>(-2) || r == 0) {
    len = 1;
    return static_cast<unsigned char>(s[i]);
  }
  len = r;
  return wc;
}

// Append the character wc to out in the current locale's encoding (a single
// First character of s (its whole multibyte sequence), or "" if s is empty --
// used as the `$*'/`${a[*]}' join separator (bash joins with IFS's first
// CHARACTER, not its first byte).
std::string mb_first_char(const std::string &s) {
  if (s.empty()) return std::string();
  size_t len = 1;
  mb_decode(s, 0, len);
  return s.substr(0, len);
}

// byte in a unibyte locale; wcrtomb otherwise).  `orig' is the source bytes,
// appended verbatim if wc is unencodable so the character is never lost.
static void mb_append_char(std::string &out, wchar_t wc, const std::string &orig) {
  if (MB_CUR_MAX <= 1) { out += static_cast<char>(wc); return; }
  char buf[MB_LEN_MAX];
  std::mbstate_t st{};
  size_t r = std::wcrtomb(buf, wc, &st);
  if (r == static_cast<size_t>(-1)) out += orig;
  else out.append(buf, r);
}

// Case-fold each character of val that matches pat (an empty pat matches all);
// `all' folds every match, otherwise only the first.  `mode': 'U' upper, 'L'
// lower, 'T' toggle.  Character-aware under a multibyte locale (towupper/
// towlower), byte-wise in the C locale.  `match' tests one character (as its
// source bytes) against pat.
static std::string mb_case_fold(const std::string &val, const std::string &pat,
                                bool all, char mode,
                                const std::function<bool(const std::string &)> &match) {
  std::string out;
  bool first = true;
  size_t i = 0;
  while (i < val.size()) {
    size_t len = 1;
    wchar_t wc = mb_decode(val, i, len);
    std::string cs = val.substr(i, len);
    bool m = pat.empty() ? true : match(cs);
    if (m && (all || first)) {
      wint_t w = static_cast<wint_t>(wc);
      wchar_t nw = wc;
      if (mode == 'U') nw = static_cast<wchar_t>(std::towupper(w));
      else if (mode == 'L') nw = static_cast<wchar_t>(std::towlower(w));
      else if (std::iswupper(w)) nw = static_cast<wchar_t>(std::towlower(w));
      else if (std::iswlower(w)) nw = static_cast<wchar_t>(std::towupper(w));
      if (nw != wc) mb_append_char(out, nw, cs);
      else out += cs;
    } else {
      out += cs;
    }
    if (!all && first) first = false;
    i += len;
  }
  return out;
}

// Encode a Unicode code point for \u / \U, matching bash's u32cconv: in the
// active LC_CTYPE locale's charset when it is not UTF-8 (Big5, EUC, ...), with
// UTF-8 as both the common case and the can't-convert fallback.  wcrtomb is
// no use here: on the BSDs (macOS included) wchar_t in a Big5 locale is the
// zero-extended multibyte value, not a Unicode code point, so like bash we
// convert the UTF-8 form with iconv.
void append_utf8_raw(std::string &out, unsigned long v);

void append_utf8(std::string &out, unsigned long v) {
#ifdef GNASH_HAVE_ICONV
  if (v > 0x7f) {
    const char *cs = nl_langinfo(CODESET);
    if (cs && *cs && std::strcmp(cs, "UTF-8") != 0) {
      // Convert the UTF-8 form to the locale charset; an unknown charset
      // falls back to ASCII (bash: "We assume ASCII when presented with an
      // unknown encoding").  Only a failed iconv_open yields raw UTF-8; a
      // conversion failure for this particular character yields the ISO C99
      // escape text instead (bash u32tocesc: \uXXXX below 0x10000, \UXXXXXXXX
      // above, uppercase hex).
      iconv_t cd = iconv_open(cs, "UTF-8");
      if (cd == reinterpret_cast<iconv_t>(-1)) cd = iconv_open("ASCII", "UTF-8");
      if (cd != reinterpret_cast<iconv_t>(-1)) {
        std::string u8;
        append_utf8_raw(u8, v);
        char inbuf[8], outbuf[25];
        std::memcpy(inbuf, u8.data(), u8.size());
        char *ip = inbuf, *op = outbuf;
        size_t il = u8.size(), ol = sizeof(outbuf);
        size_t r = iconv(cd, &ip, &il, &op, &ol);
        iconv_close(cd);
        if (r != static_cast<size_t>(-1) && il == 0) {
          out.append(outbuf, sizeof(outbuf) - ol);
        } else {
          char esc[16];
          std::snprintf(esc, sizeof(esc), v < 0x10000 ? "\\u%04lX" : "\\U%08lX", v);
          out += esc;
        }
        return;
      }
    }
  }
#endif
  append_utf8_raw(out, v);
}

// The plain UTF-8 encoding (also the fallback when iconv can't convert).
void append_utf8_raw(std::string &out, unsigned long v) {
  if (v <= 0x7f) {
    out += static_cast<char>(v);
  } else if (v <= 0x7ff) {
    out += static_cast<char>(0xc0 | (v >> 6));
    out += static_cast<char>(0x80 | (v & 0x3f));
  } else if (v <= 0xffff) {
    out += static_cast<char>(0xe0 | (v >> 12));
    out += static_cast<char>(0x80 | ((v >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (v & 0x3f));
  } else {
    out += static_cast<char>(0xf0 | (v >> 18));
    out += static_cast<char>(0x80 | ((v >> 12) & 0x3f));
    out += static_cast<char>(0x80 | ((v >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (v & 0x3f));
  }
}

// ANSI-C dequoting for $'...' -- matches bash's ansicstr(..., flags=2).  Beyond
// the single-letter escapes it decodes octal (\nnn), hex (\xHH and \x{...}),
// Unicode (\uHHHH, \UHHHHHHHH), and control (\cX) escapes into real bytes.  An
// unrecognized escape keeps its backslash, as bash does.
std::string ansi_c(const std::string &s) {
  auto hexval = [](char c) {
    return c <= '9' ? c - '0' : (std::tolower((unsigned char)c) - 'a' + 10);
  };
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] != '\\' || i + 1 >= s.size()) { out += s[i]; continue; }
    char c = s[++i];
    switch (c) {
      case 'n': out += '\n'; break;
      case 't': out += '\t'; break;
      case 'r': out += '\r'; break;
      case 'a': out += '\a'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'v': out += '\v'; break;
      case '\\': out += '\\'; break;
      case '\'': out += '\''; break;
      case '"': out += '"'; break;
      case '?': out += '?'; break;
      case 'e': case 'E': out += '\033'; break;
      case '0': case '1': case '2': case '3':
      case '4': case '5': case '6': case '7': {
        // Octal: this digit plus up to two more (three total).
        int v = c - '0', k = 0;
        while (k < 2 && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7') {
          v = v * 8 + (s[++i] - '0');
          k++;
        }
        out += static_cast<char>(v & 0xff);
        break;
      }
      case 'x': {
        // Hex: \xHH (1-2 digits) or brace form \x{HH...}.  A bare \x with no
        // hex digit is passed through unchanged; a brace form with no digits
        // terminates the whole string (bash's ansicstr bails out).
        bool brace = i + 1 < s.size() && s[i + 1] == '{';
        if (brace) i++;
        int v = 0, k = 0;
        int limit = brace ? INT_MAX : 2;
        while (k < limit && i + 1 < s.size() &&
               std::isxdigit((unsigned char)s[i + 1])) {
          v = v * 16 + hexval(s[++i]);
          k++;
        }
        if (brace && i + 1 < s.size() && s[i + 1] == '}') i++;
        if (brace && k == 0) return out;  // \x{ with no digits: drop the rest
        if (k == 0) { out += '\\'; out += 'x'; }
        else out += static_cast<char>(v & 0xff);
        break;
      }
      case 'u': case 'U': {
        // Unicode: \u up to 4 hex digits, \U up to 8; encoded as UTF-8.  With
        // no hex digit following, the backslash is kept.
        int limit = c == 'u' ? 4 : 8;
        unsigned long v = 0;
        int k = 0;
        while (k < limit && i + 1 < s.size() &&
               std::isxdigit((unsigned char)s[i + 1])) {
          v = v * 16 + static_cast<unsigned long>(hexval(s[++i]));
          k++;
        }
        if (k == 0) { out += '\\'; out += c; }
        else append_utf8(out, v);
        break;
      }
      case 'c': {
        // Control char: \cX -> TOCTRL(X).  At end of string, literal \c.
        if (i + 1 >= s.size()) { out += '\\'; out += 'c'; break; }
        char n = s[++i];
        if (n == '\\' && i + 1 < s.size() && s[i + 1] == '\\') i++;  // $'\c\\'
        out += static_cast<char>(n == '?' ? 0x7f
                                          : (std::toupper((unsigned char)n) & 0x1f));
        break;
      }
      default: out += '\\'; out += c; break;
    }
  }
  return out;
}

}  // namespace

// Character-aware whole-string case folding (exported for `declare -u/-l/-c').
// mb_capitalize upper-cases the first character and lower-cases the rest, as in
// bash's att_capcase.  mb_case_fold above (anonymous namespace) is visible here.
std::string mb_upper(const std::string &s) {
  return mb_case_fold(s, "", true, 'U', [](const std::string &) { return true; });
}
std::string mb_lower(const std::string &s) {
  return mb_case_fold(s, "", true, 'L', [](const std::string &) { return true; });
}
std::string mb_capitalize(const std::string &s) {
  return mb_case_fold(mb_lower(s), "", false, 'U',
                      [](const std::string &) { return true; });
}

// Expand a leading ~ / ~user / ~+ / ~- in WORD (the unquoted case).
static std::string expand_leading_tilde(Shell &sh, const std::string &w) {
  if (w.empty() || w[0] != '~') return w;
  size_t slash = w.find('/');
  std::string prefix = w.substr(1, (slash == std::string::npos ? w.size() : slash) - 1);
  std::string home;
  if (prefix.empty()) {
    home = sh.get("HOME");
    if (home.empty()) { const char *h = getenv("HOME"); if (h) home = h; }
  } else if (prefix == "+") {
    home = sh.get("PWD");
  } else if (prefix == "-") {
    home = sh.get("OLDPWD");
  } else if (prefix[0] == '+' || prefix[0] == '-' ||
             std::isdigit(static_cast<unsigned char>(prefix[0]))) {
    // ~N / ~+N / ~-N: the Nth directory-stack entry (bash's DIRSTACK).
    const char *dp = prefix.c_str();
    bool from_end = (*dp == '-');
    if (*dp == '+' || *dp == '-') dp++;
    bool alldig = *dp != '\0';
    for (const char *q = dp; *q; q++)
      if (!std::isdigit(static_cast<unsigned char>(*q))) alldig = false;
    if (alldig) {
      std::vector<std::string> ds = sh.dirstack();
      long k = std::atol(dp);
      long idx = from_end ? static_cast<long>(ds.size()) - 1 - k : k;
      if (idx >= 0 && static_cast<size_t>(idx) < ds.size()) home = ds[static_cast<size_t>(idx)];
      else return w;  // out of range: leave literal
    } else {
      struct passwd *pw = getpwnam(prefix.c_str());
      if (pw) home = pw->pw_dir;
    }
  } else {
    struct passwd *pw = getpwnam(prefix.c_str());
    if (pw) home = pw->pw_dir;
  }
  if (home.empty()) return w;
  // The substituted home directory is not re-scanned for expansions: escape
  // the shell specials so the later process() pass keeps them literal (e.g.
  // HOME='/usr/$x' must not expand `$x').
  std::string esc;
  for (char c : home) {
    if (c == '$' || c == '`' || c == '\\' || c == '"' || c == '\'' ||
        c == '*' || c == '?' || c == '[' || c == '~' || c == ' ' || c == '\t')
      esc += '\\';
    esc += c;
  }
  return esc + (slash == std::string::npos ? std::string() : w.substr(slash));
}

// Tilde expansion for an assignment value: leading ~ and ~ after each unquoted colon.
static std::string tilde_assign(Shell &sh, const std::string &text) {
  std::string out, cur;
  bool sq = false, dq = false;
  for (char c : text) {
    if (c == '\'' && !dq) { sq = !sq; cur += c; continue; }
    if (c == '"' && !sq) { dq = !dq; cur += c; continue; }
    if (c == ':' && !sq && !dq) { out += expand_leading_tilde(sh, cur); out += ':'; cur.clear(); continue; }
    cur += c;
  }
  out += expand_leading_tilde(sh, cur);
  return out;
}

std::string Expander::param_value(const std::string &name, bool &set, bool defaulting_op) {
  set = true;
  if (name == "?") return std::to_string(sh_.last_status);
  if (name == "$") return sh_.get("$");
  if (name == "!") {
    // $! is UNSET until an asynchronous job has run: `set -u; echo $!' is an
    // unbound-variable error (posixexp1.sub), and ${!-word} substitutes.
    if (sh_.last_bg_pid == 0) {
      set = false;
      if (sh_.opt_nounset && !defaulting_op) {
        std::fprintf(stderr, "%s!: unbound variable\n", sh_.err_prefix().c_str());
        sh_.exiting = true;
        sh_.exit_status = 127;
      }
      return std::string();
    }
    return std::to_string(sh_.last_bg_pid);
  }
  if (name == "#") return std::to_string(sh_.positional.size());
  if (name == "0") return sh_.arg0;
  if (name == "-") {
    // The current option flags, in bash's order: the single-letter set options
    // (alphabetical), then the always-on defaults B/H/m, then the invocation
    // letter (`c' for -c, `s' when reading from stdin).  hashall (h) and
    // braceexpand (B) are on by default, as in bash.
    std::string f;
    if (sh_.opt_errexit) f += 'e';
    if (sh_.opt_noglob) f += 'f';
    if (sh_.opt_hashall) f += 'h'; // hashall: on by default, dropped by `set +h'
    if (sh_.interactive) f += 'i';  // rc files test `case $- in *i*)'
    if (sh_.opt_keyword) f += 'k';  // keyword: assignments anywhere
    if (sh_.job_control) f += 'm';  // monitor: interactive job control
    if (sh_.opt_noexec) f += 'n';
    if (sh_.opt_nounset) f += 'u';
    if (sh_.opt_verbose) f += 'v';
    if (sh_.opt_xtrace) f += 'x';
    f += 'B';                       // braceexpand: on by default
    if (sh_.opt_noclobber) f += 'C';   // noclobber: `>' won't overwrite a file
    if (sh_.opt_histexpand) f += 'H';  // histexpand (set -H; interactive default)
    if (sh_.opt_physical) f += 'P';    // physical: resolve symlinks in cd/pwd
    if (sh_.invocation_char) f += sh_.invocation_char;
    return f;
  }
  if (name == "*" || name == "@") {
    // Unset exactly when there are no positional parameters (so ${*-x}
    // defaults only for $# == 0); the value joins with the first IFS char.
    set = !sh_.positional.empty();
    std::string ifs = sh_.ifs();
    std::string sep = ifs.empty() ? std::string(" ") : mb_first_char(ifs);
    std::string r;
    for (size_t k = 0; k < sh_.positional.size(); k++) {
      if (k) r += sep;
      r += sh_.positional[k];
    }
    return r;
  }
  if (!name.empty() && std::isdigit(static_cast<unsigned char>(name[0]))) {
    size_t idx = static_cast<size_t>(std::atoi(name.c_str()));
    if (idx >= 1 && idx <= sh_.positional.size()) return sh_.positional[idx - 1];
    set = false;
    // `set -u': an unset positional is an unbound-variable error too
    // (`sh -uc 'echo $1'' -- posixexp1.sub).
    if (sh_.opt_nounset && !defaulting_op) {
      std::fprintf(stderr, "%s$%s: unbound variable\n", sh_.err_prefix().c_str(), name.c_str());
      sh_.exiting = true;
      sh_.exit_status = 127;
    }
    return std::string();
  }
  std::string v;
  if (sh_.get_if_set(name, v)) return v;
  if (sh_.dynamic_var(name, v)) return v;  // RANDOM/SECONDS/LINENO/BASHPID/EPOCH*
  set = false;
  if (sh_.opt_nounset && !defaulting_op) {
    std::fprintf(stderr, "%s%s: unbound variable\n", sh_.err_prefix().c_str(), name.c_str());
    sh_.exiting = true;
    sh_.exit_status = 127;  // bash exits a non-interactive shell with 127 here
  }
  return std::string();
}

// Expand a ${...} body (without the braces).  Returns the value; sets `split`
// if the result is subject to word splitting (always, for consistency here).
static std::string expand_brace_body(Expander &, Shell &, const std::string &, bool dq);
static std::string apply_param_op(Expander &, Shell &, const std::string &name,
                                  std::string val, bool set, const std::string &rest, bool dq,
                                  bool have_sub = false, const std::string &sub = std::string(),
                                  bool top_level = false);
// Build the ${a[@]@K} "key value ..." string for array NAME (defined below).
static std::string kv_build_K(Shell &sh, const std::string &name, bool assoc);

// Detect NAME[@] / NAME[*] with an optional leading `#' (count) or `!' (keys).
static bool array_ref(const std::string &body, char &lead, std::string &name, char &sel) {
  size_t p = 0;
  lead = 0;
  if (!body.empty() && (body[0] == '#' || body[0] == '!')) { lead = body[0]; p = 1; }
  size_t s = p;
  while (p < body.size() && (std::isalnum(static_cast<unsigned char>(body[p])) || body[p] == '_')) p++;
  name = body.substr(s, p - s);
  if (name.empty()) return false;
  if (p + 3 == body.size() && body[p] == '[' && (body[p + 1] == '@' || body[p + 1] == '*') &&
      body[p + 2] == ']') {
    sel = body[p + 1];
    return true;
  }
  return false;
}

// True when S is a whole-array splat reference `BASE[@]' / `BASE[*]' (BASE a
// non-empty identifier-shaped prefix); fills BASE and SEL ('@' or '*').
static bool is_array_splat(const std::string &s, std::string &base, char &sel) {
  size_t lb = s.find('[');
  if (lb == std::string::npos || lb == 0 || s.size() != lb + 3 || s.back() != ']')
    return false;
  char c = s[lb + 1];
  if (c != '@' && c != '*') return false;
  base = s.substr(0, lb);
  sel = c;
  return true;
}

// A nameref whose target is a whole-array splat (`declare -n ref=arr[@]'):
// expanding $ref / ${ref} yields every element like ${arr[@]}.  Returns true and
// fills BASE and SEL ('@' or '*') when NAME is such a nameref.
static bool nameref_array_splat(Shell &sh, const std::string &name, std::string &base,
                                char &sel) {
  auto it = sh.vars.find(name);
  if (it == sh.vars.end() || !it->second.nameref) return false;
  return is_array_splat(it->second.value, base, sel);
}

// Detect NAME[@]OP / NAME[*]OP where OP is an operator applied element-wise
// (case-mod ^ , ; pattern removal # % ; substitution / ; transform @).  Slicing
// (:offset) and default (:-) forms act on the array as a whole and are excluded.
static bool array_op_ref(const std::string &body, std::string &name, char &sel,
                         std::string &rest) {
  if (body.empty() || !(std::isalpha(static_cast<unsigned char>(body[0])) || body[0] == '_'))
    return false;
  size_t p = 0;
  while (p < body.size() && (std::isalnum(static_cast<unsigned char>(body[p])) || body[p] == '_')) p++;
  name = body.substr(0, p);
  if (p + 3 > body.size() || body[p] != '[' ||
      (body[p + 1] != '@' && body[p + 1] != '*') || body[p + 2] != ']')
    return false;
  sel = body[p + 1];
  rest = body.substr(p + 3);
  if (rest.empty()) return false;
  char c = rest[0];
  return c == '^' || c == ',' || c == '~' || c == '#' || c == '%' || c == '/' ||
         c == '@';
}

// Find the top-level `:' separating a substring OFFSET from its LENGTH,
// skipping a colon that belongs to a `?:' ternary in the offset expression
// (`${PARAM:1 ? 4 : 2}') and any colon inside nested ${...}, $(...), (...),
// or [...] -- bash's skiparith.  npos when there is no length.
static size_t length_colon(const std::string &s) {
  int depth = 0, quest = 0;
  for (size_t i = 0; i < s.size(); i++) {
    char c = s[i];
    if (c == '$' && i + 1 < s.size() && (s[i + 1] == '{' || s[i + 1] == '(')) {
      depth++; i++; continue;
    }
    if (c == '(' || c == '[' || c == '{') { depth++; continue; }
    if (c == ')' || c == ']' || c == '}') { if (depth) depth--; continue; }
    if (depth) continue;
    if (c == '?') { quest++; continue; }
    if (c == ':') {
      if (quest) { quest--; continue; }
      return i;
    }
  }
  return std::string::npos;
}

// Detect array/positional slicing: NAME[@]:off[:len] / NAME[*]:off[:len] and
// the positional @:off[:len] / *:off[:len].  offx/lenx are the raw arithmetic
// offset and length; haslen indicates whether a length was given.
static bool slice_ref(const std::string &body, std::string &name, char &sel,
                      std::string &offx, std::string &lenx, bool &haslen) {
  size_t p = 0;
  if (!body.empty() && (body[0] == '@' || body[0] == '*') && body.size() > 1 && body[1] == ':') {
    name.clear();
    sel = body[0];
    p = 1;
  } else {
    if (body.empty() || !(std::isalpha(static_cast<unsigned char>(body[0])) || body[0] == '_'))
      return false;
    while (p < body.size() && (std::isalnum(static_cast<unsigned char>(body[p])) || body[p] == '_')) p++;
    if (p + 4 > body.size() || body[p] != '[' ||
        (body[p + 1] != '@' && body[p + 1] != '*') || body[p + 2] != ']' || body[p + 3] != ':')
      return false;
    name = body.substr(0, p);
    sel = body[p + 1];
    p += 3;
  }
  if (p >= body.size() || body[p] != ':') return false;
  std::string tail = body.substr(p + 1);
  size_t colon = length_colon(tail);
  if (colon == std::string::npos) { offx = tail; haslen = false; }
  else { offx = tail.substr(0, colon); lenx = tail.substr(colon + 1); haslen = true; }
  return true;
}

// Detect NAME[@]OP / NAME[*]OP where OP is a defaulting/alternative/error
// operator (`-` `:-` `=` `:=` `+` `:+` `?` `:?`) applied to the array as a
// whole.  Must be tried before slice_ref, which would otherwise misread
// `:-word' as a slice with a bogus `-word' offset.
static bool array_default_ref(const std::string &body, std::string &name, char &sel,
                              std::string &rest) {
  if (body.empty() || !(std::isalpha(static_cast<unsigned char>(body[0])) || body[0] == '_'))
    return false;
  size_t p = 0;
  while (p < body.size() && (std::isalnum(static_cast<unsigned char>(body[p])) || body[p] == '_')) p++;
  if (p + 3 > body.size() || body[p] != '[' ||
      (body[p + 1] != '@' && body[p + 1] != '*') || body[p + 2] != ']')
    return false;
  name = body.substr(0, p);
  sel = body[p + 1];
  rest = body.substr(p + 3);
  if (rest.empty()) return false;
  char c = rest[0];
  if (c == '-' || c == '=' || c == '+' || c == '?') return true;
  return c == ':' && rest.size() > 1 &&
         (rest[1] == '-' || rest[1] == '=' || rest[1] == '+' || rest[1] == '?');
}

bool Expander::emit_zsh_flags(const std::string &body, bool dq, std::string &out,
                              std::string &mask) {
  if (body.empty() || body[0] != '(') return false;
  size_t rp = body.find(')');
  if (rp == std::string::npos) return false;
  std::string flags = body.substr(1, rp - 1);
  std::string rest = body.substr(rp + 1);
  if (rest.empty()) return false;

  bool f_join = false, f_split = false, f_sort = false, f_rev = false, f_num = false;
  bool f_ci = false, f_uniq = false, f_keys = false, f_vals = false;
  int f_case = 0;  // 1=L 2=U 3=C
  std::string jsep, ssep;
  for (size_t p = 0; p < flags.size(); p++) {
    char c = flags[p];
    if ((c == 'j' || c == 's') && p + 1 < flags.size()) {
      char d = flags[p + 1];
      size_t q = p + 2;
      std::string sep;
      while (q < flags.size() && flags[q] != d) sep += flags[q++];
      if (c == 'j') { jsep = sep; f_join = true; } else { ssep = sep; f_split = true; }
      p = q;  // at the closing delimiter; loop ++ steps past it
      continue;
    }
    switch (c) {
      case 'F': jsep = "\n"; f_join = true; break;
      case 'f': ssep = "\n"; f_split = true; break;
      case 'o': f_sort = true; break;
      case 'O': f_sort = true; f_rev = true; break;
      case 'n': f_num = true; break;
      case 'i': f_ci = true; break;
      case 'u': f_uniq = true; break;
      case 'L': f_case = 1; break;
      case 'U': f_case = 2; break;
      case 'C': f_case = 3; break;
      case 'k': f_keys = true; break;
      case 'v': f_vals = true; break;
      default: break;  // unrecognized flags are ignored
    }
  }

  // Resolve the base parameter (name, name[sub], or @/*) to a list of values.
  size_t q = 0;
  std::string nm;
  if (rest[0] == '@' || rest[0] == '*') { nm = rest.substr(0, 1); q = 1; }
  else { while (q < rest.size() && (std::isalnum((unsigned char)rest[q]) || rest[q] == '_')) q++;
         nm = rest.substr(0, q); }
  std::string sub;
  bool have_sub = false;
  if (q < rest.size() && rest[q] == '[') {
    size_t s = q + 1, p2 = q + 1;
    int d = 1;
    while (p2 < rest.size() && d) { if (rest[p2] == '[') d++; else if (rest[p2] == ']') d--; if (d) p2++; }
    sub = rest.substr(s, p2 - s);
    have_sub = true;
  }

  std::vector<std::string> items;
  if (nm.empty()) {
    // The operand is a nested expansion, e.g. ${(s:+:)$(cmd)} or ${(f)${x}};
    // expand it to a scalar and let the split/transform flags act on it.
    items.push_back(expand_no_split(rest));
  } else if (nm == "@" || nm == "*") {
    items = sh_.positional;
  } else if (f_keys && f_vals) {
    std::vector<std::string> k = sh_.array_keys(nm), v = sh_.array_values(nm);
    for (size_t x = 0; x < k.size() && x < v.size(); x++) { items.push_back(k[x]); items.push_back(v[x]); }
  } else if (f_keys) {
    items = sh_.array_keys(nm);
  } else if (have_sub && sub != "@" && sub != "*") {
    items.push_back(sh_.array_get(nm, sh_.zsh_subscript(nm, expand_no_split(sub))));
  } else if (sh_.is_array(nm)) {
    items = sh_.array_values(nm);
  } else {
    items.push_back(sh_.get(nm));
  }

  // split (s/f): split each element on the separator.
  if (f_split) {
    std::vector<std::string> out2;
    for (const std::string &it : items) {
      if (ssep.empty()) { out2.push_back(it); continue; }
      size_t pos = 0, nx;
      while ((nx = it.find(ssep, pos)) != std::string::npos) { out2.push_back(it.substr(pos, nx - pos)); pos = nx + ssep.size(); }
      out2.push_back(it.substr(pos));
    }
    items.swap(out2);
  }
  // case (L/U/C).
  if (f_case) {
    for (std::string &it : items) {
      if (f_case == 1) for (char &c : it) c = std::tolower((unsigned char)c);
      else if (f_case == 2) for (char &c : it) c = std::toupper((unsigned char)c);
      else {  // C: capitalize the first letter of each word
        bool start = true;
        for (char &c : it) {
          if (std::isalnum((unsigned char)c)) { c = start ? std::toupper((unsigned char)c) : std::tolower((unsigned char)c); start = false; }
          else start = true;
        }
      }
    }
  }
  // unique (u): drop later duplicates, keep first-seen order.
  if (f_uniq) {
    std::vector<std::string> out2;
    for (const std::string &it : items)
      if (std::find(out2.begin(), out2.end(), it) == out2.end()) out2.push_back(it);
    items.swap(out2);
  }
  // sort (o/O), optionally numeric (n) / case-insensitive (i).
  if (f_sort) {
    auto lower = [](std::string s) { for (char &c : s) c = std::tolower((unsigned char)c); return s; };
    std::stable_sort(items.begin(), items.end(), [&](const std::string &a, const std::string &b) {
      if (f_num) { long long x = std::atoll(a.c_str()), y = std::atoll(b.c_str()); if (x != y) return x < y; }
      if (f_ci) return lower(a) < lower(b);
      return a < b;
    });
    if (f_rev) std::reverse(items.begin(), items.end());
  }
  // join (j/F): collapse to a single scalar.
  if (f_join) {
    std::string joined;
    for (size_t x = 0; x < items.size(); x++) { if (x) joined += jsep; joined += items[x]; }
    items.assign(1, joined);
    char qm = dq ? '1' : '0';
    for (char c : joined) { out += c; mask += qm; }
    return true;
  }
  // Emit the list: one word per element (unquoted) or IFS-joined (double-quoted).
  if (dq) {
    std::string is = sh_.ifs();
    std::string joiner = mb_first_char(is);
    for (size_t x = 0; x < items.size(); x++) {
      if (x) for (char c : joiner) { out += c; mask += '1'; }
      for (char c : items[x]) { out += c; mask += '1'; }
    }
  } else {
    for (size_t x = 0; x < items.size(); x++) {
      if (x) { out += FIELD_SEP; mask += MMARK; }
      for (char c : items[x]) { out += c; mask += '0'; }
    }
  }
  return true;
}

void Expander::emit_zsh_subscript(const std::string &name, const std::string &sub, bool dq,
                                  std::string &out, std::string &mask) {
  // A top-level comma makes this a zsh range `lo,hi' (both 1-based, inclusive,
  // negatives counting from the end).  Otherwise it is a single index.
  size_t comma = std::string::npos;
  for (size_t k = 0; k < sub.size(); k++) {
    if (sub[k] == '[') { int d = 1; while (++k < sub.size() && d) d += (sub[k]=='[') - (sub[k]==']'); }
    else if (sub[k] == ',') { comma = k; break; }
  }
  char qm = dq ? '1' : '0';
  // Scalar subscripting is character (single) / substring (range) selection,
  // 1-based, negatives counting from the end -- e.g. s=hello, $s[1]=h, $s[2,4]=ell.
  if (!sh_.is_array(name)) {
    std::string val = sh_.get(name);
    long long n = static_cast<long long>(val.size());
    bool ok = true;
    long long lo, hi;
    if (comma != std::string::npos) {
      lo = eval_arith(sh_, expand_no_split(sub.substr(0, comma), false, false), &ok);
      hi = eval_arith(sh_, expand_no_split(sub.substr(comma + 1), false, false), &ok);
    } else {
      lo = hi = eval_arith(sh_, expand_no_split(sub, false, false), &ok);
    }
    if (lo < 0) lo += n + 1;
    if (hi < 0) hi += n + 1;
    if (lo < 1) lo = 1;
    for (long long k = lo; k <= hi && k <= n; k++)
      if (k >= 1) { out += val[static_cast<size_t>(k - 1)]; mask += qm; }
    return;
  }
  if (comma != std::string::npos) {
    std::vector<std::string> all = sh_.array_values(name);
    long long n = static_cast<long long>(all.size());
    bool ok = true;
    long long lo = eval_arith(sh_, expand_no_split(sub.substr(0, comma), false, false), &ok);
    long long hi = eval_arith(sh_, expand_no_split(sub.substr(comma + 1), false, false), &ok);
    if (lo < 0) lo += n + 1;  // -1 == last element (position n)
    if (hi < 0) hi += n + 1;
    std::vector<std::string> items;
    for (long long k = lo; k <= hi; k++)
      if (k >= 1 && k <= n) items.push_back(all[static_cast<size_t>(k - 1)]);
    if (dq) {
      std::string is = sh_.ifs();
      std::string joiner = mb_first_char(is);
      for (size_t k = 0; k < items.size(); k++) {
        if (k) for (char c : joiner) { out += c; mask += '1'; }
        for (char c : items[k]) { out += c; mask += '1'; }
      }
    } else {
      for (size_t k = 0; k < items.size(); k++) {
        if (k) { out += FIELD_SEP; mask += MMARK; }
        for (char c : items[k]) { out += c; mask += '0'; }
      }
    }
    return;
  }
  std::string val = sh_.array_get(name, sh_.zsh_subscript(name, expand_no_split(sub)));
  for (char c : val) { out += c; mask += qm; }
}

void Expander::emit_array_items(const std::vector<std::string> &items, char sel, bool dq,
                                std::string &out, std::string &mask) {
  if (sel == '@' && dq) {
    // "${a[@]}": one field per element (empties kept); absorb the quoted-null
    // the opening quote emitted so an empty list drops the word.
    if (!out.empty() && out.back() == QNULL && mask.back() == MMARK) {
      out.pop_back();
      mask.pop_back();
    }
    for (size_t k = 0; k < items.size(); k++) {
      if (k) { out += FIELD_SEP; mask += MMARK; }
      out += QNULL; mask += MMARK;
      for (char c : items[k]) { out += c; mask += '1'; }
    }
  } else if (sel == '*' && dq) {
    std::string is = sh_.ifs();
    std::string j = mb_first_char(is);
    for (size_t k = 0; k < items.size(); k++) {
      if (k) for (char c : j) { out += c; mask += '1'; }
      for (char c : items[k]) { out += c; mask += '1'; }
    }
  } else if (sel == '*' && splitting_ && sh_.ifs().empty()) {
    bool first = true;
    for (size_t k = 0; k < items.size(); k++) {
      if (items[k].empty()) continue;
      if (!first) { out += FIELD_SEP; mask += MMARK; }
      first = false;
      for (char c : items[k]) { out += c; mask += '0'; }
    }
  } else if (sel == '*') {
    std::string is = sh_.ifs();
    std::string j = mb_first_char(is);
    for (size_t k = 0; k < items.size(); k++) {
      if (k) for (char c : j) { out += c; mask += '0'; }
      for (char c : items[k]) { out += c; mask += '0'; }
    }
  } else {
    bool first = true;
    for (size_t k = 0; k < items.size(); k++) {
      if (splitting_ && items[k].empty()) continue;
      if (!first) { out += FIELD_SEP; mask += MMARK; }
      first = false;
      for (char c : items[k]) { out += c; mask += '0'; }
    }
  }
}

void Expander::expand_dollar(const std::string &t, size_t &i, bool dq, std::string &out,
                             std::string &mask, bool heredoc) {
  char qm = dq ? '1' : '0';
  // i is at '$'
  char n1 = i + 1 < t.size() ? t[i + 1] : '\0';
  // A double-quoted "$@"/"${a[@]}" manages its own fields, so it absorbs the
  // quoted-null the opening quote emitted -- letting an empty list drop the word.
  auto absorb_qnull = [&]() {
    if (!out.empty() && out.back() == QNULL && mask.back() == MMARK) {
      out.pop_back();
      mask.pop_back();
    }
  };

  // $((expr)) -- but `$((cmd);(cmd))' is the command substitution `$( (...)':
  // it is ARITHMETIC only when the paren depth never falls below 2 before the
  // closing `))', i.e. no top-level `)' appears inside (bash's rule; probe:
  // `$((echo a);(echo b))' runs the subshells, `$((echo hi))' is an
  // arithmetic syntax error).
  if (n1 == '(' && i + 2 < t.size() && t[i + 2] == '(') {
    size_t p = i + 3;
    int depth = 2;
    size_t end = std::string::npos;
    bool cmd_sub = false;
    for (; p < t.size(); p++) {
      if (t[p] == '(') depth++;
      else if (t[p] == ')') {
        if (--depth == 0) { end = p; break; }
        if (depth == 1 && !(p + 1 < t.size() && t[p + 1] == ')'))
          cmd_sub = true;  // a top-level `)' not immediately closing the whole span
      }
    }
    if (cmd_sub) end = std::string::npos;  // fall through to the $(cmd) branch
    if (end != std::string::npos) {
      std::string expr = t.substr(i + 3, (end - 1) - (i + 3));
      bool ok = true;
      long long v = eval_arith_msg(sh_, expand_arith(expr), "", &ok, /*expand_subs=*/1);
      // A $((...)) syntax error unwinds the whole command LIST (bash's
      // DISCARD longjmp): `-c 'echo $((x+)); exit 0'' exits 1 without
      // running the exit -- while a file reader continues at the next line.
      // In POSIX mode the expansion error is fatal to a non-interactive
      // shell outright (bash exits 127 -- posixexp2.sub test 15).
      if (!ok) {
        sh_.arith_abort = true;
        sh_.last_status = 1;
        if (sh_.opt_posix && !sh_.interactive) {
          sh_.exiting = true;
          sh_.exit_status = 127;
        }
        i = end + 1;
        return;
      }
      std::string s = std::to_string(v);
      for (char c : s) { out += c; mask += qm; }
      i = end + 1;
      return;
    }
  }
  // $[expr] -- deprecated arithmetic expansion
  if (n1 == '[') {
    size_t end = scan_balanced(t, i + 1, '[', ']');
    if (end != std::string::npos) {
      std::string expr = t.substr(i + 2, end - (i + 2));
      bool ok = true;
      long long v = eval_arith_msg(sh_, expand_arith(expr), "", &ok, /*expand_subs=*/1);
      // A $((...)) syntax error unwinds the whole command LIST (bash's
      // DISCARD longjmp): `-c 'echo $((x+)); exit 0'' exits 1 without
      // running the exit -- while a file reader continues at the next line.
      // In POSIX mode the expansion error is fatal to a non-interactive
      // shell outright (bash exits 127 -- posixexp2.sub test 15).
      if (!ok) {
        sh_.arith_abort = true;
        sh_.last_status = 1;
        if (sh_.opt_posix && !sh_.interactive) {
          sh_.exiting = true;
          sh_.exit_status = 127;
        }
        i = end + 1;
        return;
      }
      std::string s = std::to_string(v);
      for (char c : s) { out += c; mask += qm; }
      i = end + 1;
      return;
    }
  }
  // $(cmd)
  if (n1 == '(') {
    // The lexer's scanner is the single source of truth for the span: it
    // knows case patterns (`$(case x in in|esac) ...;; esac)'), comments,
    // and here-documents; fall back to the plain balanced scan if it calls
    // the span unterminated.
    size_t end = sh_.aliases_active() ? comsub_span_end_aliased(t, i + 1, sh_.aliases)
                                      : comsub_span_end(t, i + 1);
    end = (end == std::string::npos) ? scan_balanced(t, i + 1, '(', ')') : end - 1;
    if (end == std::string::npos && !heredoc) {
      // An unterminated `$(' reaching expansion (a quoted one inside a
      // ${...} operand, where single quotes are literal) is bash's
      // command-substitution parse error, and the command aborts
      // (braces.tests).
      // bash's prefix names the failing context and the line where the
      // substitution's text ran out (its own last line).
      int nl = static_cast<int>(std::count(t.begin() + static_cast<long>(i), t.end(), '\n'));
      std::fprintf(stderr, "%s: command substitution: line %d: "
                           "unexpected EOF while looking for matching `)'\n",
                   sh_.shell_name.c_str(), sh_.cur_lineno + nl + 2);
      sh_.arith_error = true;
      i = t.size();
      return;
    }
    if (end != std::string::npos) {
      std::string inner = t.substr(i + 2, end - (i + 2));
      int st = 0;
      std::string res = sh_.run_and_capture(inner, &st);
      sh_.last_status = st;
      sh_.note_cmdsub(st);
      // '4' marks command-substitution output, which zsh word-splits (unlike
      // parameter expansion); double-quoted, it is not split.
      char cm = dq ? '1' : '4';
      for (char c : res) { out += c; mask += cm; }
      i = end + 1;
      return;
    }
  }
  // ${ cmd; } -- function substitution: run in the current shell, capture
  // stdout.  Distinguished from ${var} by whitespace (or `|') after the brace.
  if (n1 == '{' && i + 2 < t.size() &&
      (std::isspace(static_cast<unsigned char>(t[i + 2])) || t[i + 2] == '|')) {
    size_t end = scan_balanced(t, i + 1, '{', '}');
    if (end != std::string::npos) {
      std::string inner = t.substr(i + 2, end - (i + 2));
      bool valsub = !inner.empty() && inner[0] == '|';  // ${| cmd; } -> $REPLY
      if (valsub) inner[0] = ' ';
      int st = 0;
      std::string res = sh_.run_and_capture_inproc(inner, &st, valsub);
      sh_.last_status = st;
      sh_.note_cmdsub(st);
      char cm = dq ? '1' : '4';  // command output: zsh-splittable (see $(...) above)
      for (char c : res) { out += c; mask += cm; }
      i = end + 1;
      return;
    }
  }
  // ${...}
  if (n1 == '{') {
    size_t end = scan_balanced(t, i + 1, '{', '}', /*firstclose=*/true,
                               /*squote_literal=*/dq && sh_.opt_posix);
    if (end != std::string::npos) {
      std::string body = t.substr(i + 2, end - (i + 2));

      // ${@} / ${*}: a bare positional list, identical to $@ / $*.  Handle it
      // here so `"${@}"' keeps its per-parameter field structure rather than
      // being flattened to a single word by the generic scalar path below.
      if (body == "@" || body == "*") {
        const auto &pos = sh_.positional;
        if (body[0] == '@' && dq) {
          absorb_qnull();
          for (size_t k = 0; k < pos.size(); k++) {
            if (k) { out += FIELD_SEP; mask += MMARK; }
            out += QNULL; mask += MMARK;
            for (char c : pos[k]) { out += c; mask += '1'; }
          }
        } else if (body[0] == '*' && dq) {
          std::string sep = sh_.ifs();
          std::string joiner = mb_first_char(sep);
          for (size_t k = 0; k < pos.size(); k++) {
            if (k) for (char c : joiner) { out += c; mask += '1'; }
            for (char c : pos[k]) { out += c; mask += '1'; }
          }
        } else if (body[0] == '*' && splitting_ && sh_.ifs().empty()) {
          bool first = true;  // empty IFS: separate fields, empties dropped
          for (size_t k = 0; k < pos.size(); k++) {
            if (pos[k].empty()) continue;
            if (!first) { out += FIELD_SEP; mask += MMARK; }
            first = false;
            for (char c : pos[k]) { out += c; mask += '0'; }
          }
        } else if (body[0] == '*') {  // unquoted (non-empty IFS) / assignment: join IFS[0]
          std::string sep = sh_.ifs();
          std::string joiner = mb_first_char(sep);
          for (size_t k = 0; k < pos.size(); k++) {
            if (k) for (char c : joiner) { out += c; mask += '0'; }
            for (char c : pos[k]) { out += c; mask += '0'; }
          }
        } else {  // unquoted ${@}: empties vanish only when splitting
          bool first = true;
          for (size_t k = 0; k < pos.size(); k++) {
            if (splitting_ && pos[k].empty()) continue;
            if (!first) { out += FIELD_SEP; mask += MMARK; }
            first = false;
            for (char c : pos[k]) { out += c; mask += '0'; }
          }
        }
        i = end + 1;
        return;
      }

      // ${name-word} / ${name+word} (and the `:' forms) with the operator
      // firing: expand WORD by re-processing it here, so an embedded "$@"
      // keeps its field structure (a flat string would lose empty fields).
      {
        size_t q = 0;
        std::string nm;
        if (q < body.size() &&
            (body[q] == '@' || body[q] == '*' || body[q] == '#' || body[q] == '?' ||
             body[q] == '$' || body[q] == '!' || body[q] == '-')) {
          nm = body.substr(q, 1);
          q++;
        } else {
          while (q < body.size() &&
                 (std::isalnum(static_cast<unsigned char>(body[q])) || body[q] == '_'))
            q++;
          nm = body.substr(0, q);
        }
        if (!nm.empty() && q < body.size()) {
          bool colon = body[q] == ':';
          size_t opq = q + (colon ? 1 : 0);
          // `${#+}' -- an operator with NO operand after the argument count --
          // is a bad substitution, so leave it for the full parser below.
          bool bare_op = nm == "#" && !colon && opq + 1 == body.size();
          if (opq < body.size() && (body[opq] == '-' || body[opq] == '+') &&
              !bare_op && !(nm == "-" && q == 1)) {
            char op = body[opq];
            bool set = false;
            std::string val = param_value(nm, set, true);
            // For `:', a scalar is null when its value is empty; but $@/$* are
            // null iff their IFS[0]-joined $* is empty -- every positional empty
            // AND the join adds nothing (at most one param, or an empty IFS).
            bool null_val;
            if (nm == "@" || nm == "*") {
              const auto &pos = sh_.positional;
              bool allempty = true;
              for (const auto &p : pos) if (!p.empty()) { allempty = false; break; }
              null_val = allempty && (pos.size() <= 1 || sh_.ifs().empty());
              set = !pos.empty();
            } else {
              null_val = val.empty();
            }
            bool fire = (op == '-') ? (!set || (colon && null_val))
                                    : (set && !(colon && null_val));
            if (fire) {
              std::string word = body.substr(opq + 1);
              // Only `$@'/`${a[@]}' keep field structure inside double quotes;
              // `$*' joins with IFS[0] (exp9.sub).
              bool has_at = word.find("$@") != std::string::npos ||
                            word.find("[@]") != std::string::npos;
              // In double quotes (bash-family), expand the word in double-quote
              // context so backslash escapes and literal quotes behave right;
              // a word with "$@"/"$*" keeps its field structure via process().
              if (dq && !sh_.is_zsh() && !has_at) {
                std::string ex = expand_dq_word(word);
                for (char c : ex) { out += c; mask += '1'; }
              } else if (heredoc) {
                // Inside ${...} in a here-document, DOUBLE quotes and
                // backslash escapes are active (`${P+\"$P\"}' emits `"A"',
                // `${P+"$P"}' emits `A') while single quotes and $'...'
                // stay literal (bash).  Neutralize the `$''/`$"' forms and
                // process as an unquoted word with literal single quotes.
                std::string w = expand_leading_tilde(sh_, word);
                std::string w2;
                for (size_t k = 0; k < w.size(); k++) {
                  if (w[k] == '\\' && k + 1 < w.size()) {
                    w2 += w[k];
                    w2 += w[k + 1];
                    k++;
                    continue;
                  }
                  if (w[k] == '$' && k + 1 < w.size() && (w[k + 1] == '\'' || w[k + 1] == '"')) {
                    w2 += "\\$";
                    continue;
                  }
                  w2 += w[k];
                }
                // Wrap in double quotes: backslash then behaves as in
                // dquotes (escaping only \ $ \` "), inner quotes toggle out
                // and vanish, and nothing word-splits -- the heredoc rules.
                process("\"" + w2 + "\"", out, mask, false, false);
              } else {
                // Unquoted default word: a leading `~' tilde-expands (bash).
                size_t m0 = mask.size();
                process(expand_leading_tilde(sh_, word), out, mask, false, false);
                // The replacement is EXPANSION OUTPUT: its unquoted literal
                // text IFS-splits like a parameter's value (`${IFS+foo 'b c'
                // baz}' is three fields, the middle protected by its quotes).
                for (size_t mk = m0; mk < mask.size(); mk++)
                  if (mask[mk] == '2') mask[mk] = '0';
              }
              i = end + 1;
              return;
            }
            if (op == '-' || op == '+') {
              // Operator does not fire.  For `@'/`*' the parameter is the
              // positional list: `-' emits the positionals (1..$#, never $0),
              // `+' emits nothing.  A plain scalar emits its own value for `-'.
              // A subscripted name falls through to the array handlers below.
              if (nm == "@" || nm == "*") {
                if (op == '-') {
                  const auto &pos = sh_.positional;
                  if (nm == "@" && dq) {
                    absorb_qnull();
                    for (size_t k = 0; k < pos.size(); k++) {
                      if (k) { out += FIELD_SEP; mask += MMARK; }
                      out += QNULL; mask += MMARK;  // keep an empty positional
                      for (char c : pos[k]) { out += c; mask += '1'; }
                    }
                  } else if (nm == "*" && dq) {
                    std::string is = sh_.ifs();
                    std::string j = mb_first_char(is);
                    for (size_t k = 0; k < pos.size(); k++) {
                      if (k) for (char c : j) { out += c; mask += '1'; }
                      for (char c : pos[k]) { out += c; mask += '1'; }
                    }
                  } else if (nm == "*" && splitting_ && sh_.ifs().empty()) {
                    bool first = true;
                    for (size_t k = 0; k < pos.size(); k++) {
                      if (pos[k].empty()) continue;
                      if (!first) { out += FIELD_SEP; mask += MMARK; }
                      first = false;
                      for (char c : pos[k]) { out += c; mask += '0'; }
                    }
                  } else if (nm == "*") {
                    std::string is = sh_.ifs();
                    std::string j = mb_first_char(is);
                    for (size_t k = 0; k < pos.size(); k++) {
                      if (k) for (char c : j) { out += c; mask += '0'; }
                      for (char c : pos[k]) { out += c; mask += '0'; }
                    }
                  } else {  // unquoted @: empties vanish only when splitting
                    bool first = true;
                    for (size_t k = 0; k < pos.size(); k++) {
                      if (splitting_ && pos[k].empty()) continue;
                      if (!first) { out += FIELD_SEP; mask += MMARK; }
                      first = false;
                      for (char c : pos[k]) { out += c; mask += '0'; }
                    }
                  }
                }
                i = end + 1;
                return;
              }
              if (body.find('[') == std::string::npos) {
                if (op == '-') {
                  char qm2 = dq ? '1' : '0';
                  for (char c : val) { out += c; mask += qm2; }
                }
                i = end + 1;
                return;
              }
            }
          }
        }
      }
      // zsh `${(flags)name}' expansion flags (join/split/sort/unique/case/...).
      if (sh_.is_zsh() && !body.empty() && body[0] == '(' &&
          emit_zsh_flags(body, dq, out, mask)) {
        i = end + 1;
        return;
      }
      // zsh `${=name}': force IFS word-splitting of the value, even in quotes.
      if (sh_.is_zsh() && body.size() > 1 && body[0] == '=') {
        std::string val = expand_brace_body(*this, sh_, body.substr(1), dq);
        std::string ifs = sh_.ifs();
        auto is_ifs = [&](char c) { return ifs.find(c) != std::string::npos; };
        auto is_ws = [&](char c) { return c == ' ' || c == '\t' || c == '\n'; };
        bool first = true;
        std::string cur;
        bool have = false;
        auto flush = [&]() {
          if (!have) return;
          if (!first) { out += FIELD_SEP; mask += MMARK; }
          for (char c : cur) { out += c; mask += '0'; }
          first = false; cur.clear(); have = false;
        };
        for (size_t p = 0; p < val.size();) {
          char c = val[p];
          if (is_ifs(c)) {
            if (is_ws(c)) { flush(); while (p < val.size() && is_ifs(val[p]) && is_ws(val[p])) p++; }
            else { flush(); p++; }
            continue;
          }
          cur += c; have = true; p++;
        }
        flush();
        i = end + 1;
        return;
      }
      // zsh `${a[lo,hi]}' array range, and scalar `${s[i]}' / `${s[i,j]}'
      // character/substring selection.  A single array `${a[i]}' stays in
      // expand_brace_body (1-based) so operators like ${a[i]:-x} still work; we
      // only intercept a bare name[..] with no trailing operator.
      if (sh_.is_zsh() && !body.empty() && body.back() == ']') {
        size_t lb = body.find('[');
        if (lb != std::string::npos && lb > 0) {
          std::string zn = body.substr(0, lb);
          bool ident = std::isalpha(static_cast<unsigned char>(zn[0])) || zn[0] == '_';
          for (size_t k = 1; ident && k < zn.size(); k++)
            ident = std::isalnum(static_cast<unsigned char>(zn[k])) || zn[k] == '_';
          std::string zsub = body.substr(lb + 1, body.size() - lb - 2);
          bool arr = sh_.is_array(zn);
          bool range = zsub.find(',') != std::string::npos;
          // array range, or any scalar subscript (single char or substring)
          if (ident && ((arr && range) || (!arr && sh_.is_set(zn)))) {
            emit_zsh_subscript(zn, zsub, dq, out, mask);
            i = end + 1;
            return;
          }
        }
      }
      // `${ref}' where ref is a nameref to a whole-array splat (`arr[@]') expands
      // like `${arr[@]}'; likewise `${!indir}' where indir's VALUE is such a
      // splat.  Rewrite the body to the target so the array path below preserves
      // the field structure (a plain string return would flatten it).  A body
      // carrying any operator won't match the bare-name lookup, so this only
      // fires for `${ref}' / `${!indir}'.
      {
        std::string nrbase;
        char nrsel;
        if (nameref_array_splat(sh_, body, nrbase, nrsel)) {
          body = nrbase + '[' + nrsel + ']';
        } else if (body.size() > 1 && body[0] == '!') {
          // ${!indir}: a non-nameref plain variable whose value is `arr[@]'.
          // (A nameref indir is special-cased to its target NAME elsewhere.)
          auto it = sh_.vars.find(body.substr(1));
          if (it != sh_.vars.end() && !it->second.nameref &&
              is_array_splat(it->second.value, nrbase, nrsel))
            body = it->second.value;
        }
      }
      char lead, sel;
      std::string aname;
      if (array_ref(body, lead, aname, sel)) {
        if (lead == '#') {
          std::string cnt = std::to_string(sh_.array_count(aname));
          for (char c : cnt) { out += c; mask += qm; }
        } else {
          std::vector<std::string> items =
              (lead == '!') ? sh_.array_keys(aname) : sh_.array_values(aname);
          if (sel == '@' && dq) {
            absorb_qnull();
            for (size_t k = 0; k < items.size(); k++) {
              if (k) { out += FIELD_SEP; mask += MMARK; }
              out += QNULL; mask += MMARK;  // keep an empty element as a field
              for (char c : items[k]) { out += c; mask += '1'; }
            }
          } else if (sel == '*' && dq) {
            std::string is = sh_.ifs();
            std::string j = mb_first_char(is);
            for (size_t k = 0; k < items.size(); k++) {
              if (k) for (char c : j) { out += c; mask += '1'; }
              for (char c : items[k]) { out += c; mask += '1'; }
            }
          } else if (sel == '*' && splitting_ && sh_.ifs().empty()) {
            // Unquoted ${a[*]} with an EMPTY IFS: separate fields (like ${a[@]}),
            // empties dropped -- the IFS[0]-join form below would otherwise
            // collapse them into one field.  See the $* case above.
            bool first = true;
            for (size_t k = 0; k < items.size(); k++) {
              if (items[k].empty()) continue;
              if (!first) { out += FIELD_SEP; mask += MMARK; }
              first = false;
              for (char c : items[k]) { out += c; mask += '0'; }
            }
          } else if (sel == '*') {
            // Assignment / no-split ${a[*]}: join with the first IFS char, left
            // splittable (mask 0) so an assignment RHS keeps it as the joiner.
            std::string is = sh_.ifs();
            std::string j = mb_first_char(is);
            for (size_t k = 0; k < items.size(); k++) {
              if (k) for (char c : j) { out += c; mask += '0'; }
              for (char c : items[k]) { out += c; mask += '0'; }
            }
          } else {
            // Unquoted ${a[@]}: in a splitting context an empty element produces
            // no word (it splits away); in a no-split/assignment context the
            // element boundaries are kept (they flatten to IFS[0] separators),
            // so only drop empties when actually splitting.
            bool first = true;
            for (size_t k = 0; k < items.size(); k++) {
              if (splitting_ && items[k].empty()) continue;
              if (!first) { out += FIELD_SEP; mask += MMARK; }
              first = false;
              for (char c : items[k]) { out += c; mask += '0'; }
            }
          }
        }
        i = end + 1;
        return;
      }
      // ${@OP} / ${*OP}: apply a per-element operator (pattern removal #/%,
      // substitution /, case-mod ^/,, transform @) to each positional param.
      if ((body[0] == '@' || body[0] == '*') && body.size() > 1 &&
          (body[1] == '#' || body[1] == '%' || body[1] == '/' || body[1] == '^' ||
           body[1] == ',' || body[1] == '~' || body[1] == '@')) {
        char psel = body[0];
        std::string prest = body.substr(1);
        std::vector<std::string> items = sh_.positional;
        for (std::string &it : items)
          it = apply_param_op(*this, sh_, std::string(1, psel), it, true, prest, dq);
        if (psel == '*' && dq) {
          std::string is = sh_.ifs();
          std::string j = mb_first_char(is);
          for (size_t k = 0; k < items.size(); k++) {
            if (k) for (char c : j) { out += c; mask += '1'; }
            for (char c : items[k]) { out += c; mask += '1'; }
          }
        } else if (psel == '*' && splitting_ && sh_.ifs().empty()) {
          bool first = true;  // empty IFS: separate fields, empties dropped
          for (size_t k = 0; k < items.size(); k++) {
            if (items[k].empty()) continue;
            if (!first) { out += FIELD_SEP; mask += MMARK; }
            first = false;
            for (char c : items[k]) { out += c; mask += '0'; }
          }
        } else if (psel == '*') {  // join with IFS[0], splittable
          std::string is = sh_.ifs();
          std::string j = mb_first_char(is);
          for (size_t k = 0; k < items.size(); k++) {
            if (k) for (char c : j) { out += c; mask += '0'; }
            for (char c : items[k]) { out += c; mask += '0'; }
          }
        } else {  // psel == '@'
          char m = dq ? '1' : '0';
          if (dq) absorb_qnull();
          bool first = true;
          for (size_t k = 0; k < items.size(); k++) {
            if (!dq && splitting_ && items[k].empty()) continue;  // vanish when splitting
            if (!first) { out += FIELD_SEP; mask += MMARK; }
            first = false;
            if (dq) { out += QNULL; mask += MMARK; }
            for (char c : items[k]) { out += c; mask += m; }
          }
        }
        i = end + 1;
        return;
      }
      // ${a[@]OP} / ${a[*]OP}: apply OP to each element.
      std::string aoname, arest;
      char asel;
      if (array_op_ref(body, aoname, asel, arest)) {
        std::vector<std::string> items;
        if (arest == "@A") {
          // Whole-array @A: a single `declare' statement recreating the array,
          // emitted as its three top-level words (`declare', `-flags',
          // `name=(...)') -- quoted it stays three words, unquoted the third is
          // then IFS-split, matching bash.  Reuses declare -p's exact rendering.
          std::string dn = sh_.deref(aoname);
          auto vit = sh_.vars.find(dn);
          if (vit != sh_.vars.end()) {
            std::string ds = declare_var_string(dn, vit->second, "declare", false);
            // Split off the leading `declare' and `-flags' words; keep the
            // `name=(...)' remainder whole (it may contain spaces).
            size_t s1 = ds.find(' ');
            size_t s2 = s1 == std::string::npos ? s1 : ds.find(' ', s1 + 1);
            if (s2 == std::string::npos) {
              items.push_back(ds);
            } else {
              items.push_back(ds.substr(0, s1));
              items.push_back(ds.substr(s1 + 1, s2 - s1 - 1));
              items.push_back(ds.substr(s2 + 1));
            }
          }
        } else if (arest == "@k" || arest == "@K") {
          // Key/value transforms.  @k emits the keys interleaved with values as
          // raw words; @K collapses to one "key value ..." string (values, and
          // assoc keys, quoted for re-input).
          std::string dn = sh_.deref(aoname);
          auto vit = sh_.vars.find(dn);
          bool is_assoc = vit != sh_.vars.end() && vit->second.kind == VarKind::Assoc;
          if (arest == "@K") {
            items.push_back(kv_build_K(sh_, aoname, is_assoc));
          } else {
            auto keys = sh_.array_keys(aoname);
            auto vals = sh_.array_values(aoname);
            for (size_t k = 0; k < keys.size(); k++) {
              items.push_back(keys[k]);
              items.push_back(k < vals.size() ? vals[k] : std::string());
            }
          }
        } else {
          items = sh_.array_values(aoname);
          for (std::string &it : items) it = apply_param_op(*this, sh_, aoname, it, true, arest, dq);
        }
        if (asel == '*' && dq) {
          std::string is = sh_.ifs();
          std::string j = mb_first_char(is);
          for (size_t k = 0; k < items.size(); k++) {
            if (k) for (char c : j) { out += c; mask += '1'; }
            for (char c : items[k]) { out += c; mask += '1'; }
          }
        } else if (asel == '@' && dq) {
          absorb_qnull();
          for (size_t k = 0; k < items.size(); k++) {
            if (k) { out += FIELD_SEP; mask += MMARK; }
            out += QNULL; mask += MMARK;  // keep empty element as a field
            for (char c : items[k]) { out += c; mask += '1'; }
          }
        } else if (asel == '*' && splitting_ && sh_.ifs().empty()) {
          bool first = true;  // empty IFS: separate fields, empties dropped
          for (size_t k = 0; k < items.size(); k++) {
            if (items[k].empty()) continue;
            if (!first) { out += FIELD_SEP; mask += MMARK; }
            first = false;
            for (char c : items[k]) { out += c; mask += '0'; }
          }
        } else if (asel == '*') {
          // Unquoted ${a[*]OP}: join with the first IFS char, left splittable.
          std::string is = sh_.ifs();
          std::string j = mb_first_char(is);
          for (size_t k = 0; k < items.size(); k++) {
            if (k) for (char c : j) { out += c; mask += '0'; }
            for (char c : items[k]) { out += c; mask += '0'; }
          }
        } else {
          // Unquoted ${a[@]OP}: an element the operator makes empty produces no
          // word when splitting; a no-split/assignment context keeps the element
          // boundaries (flattened to IFS[0] separators).
          bool first = true;
          for (size_t k = 0; k < items.size(); k++) {
            if (splitting_ && items[k].empty()) continue;
            if (!first) { out += FIELD_SEP; mask += MMARK; }
            first = false;
            for (char c : items[k]) { out += c; mask += '0'; }
          }
        }
        i = end + 1;
        return;
      }
      // ${a[@]:-word} / ${a[*]:+word} etc: a defaulting/alternative/error
      // operator on the whole array.  Handled before slice_ref (which would
      // otherwise read `:-word' as a slice offset).
      std::string dfname, dfrest; char dfsel;
      if (array_default_ref(body, dfname, dfsel, dfrest)) {
        std::vector<std::string> vals = sh_.array_values(dfname);
        bool unset = vals.empty();
        // For `:', an array is "null" iff its IFS[0]-joined ${a[*]} value is
        // empty -- i.e. every element is empty AND the join produces nothing (at
        // most one element, or an empty IFS).  Two empty elements with a
        // non-empty IFS join to a separator string, so they are NOT null (bash).
        bool allempty = true;
        for (const std::string &v : vals) if (!v.empty()) { allempty = false; break; }
        bool joined_empty = allempty && (vals.size() <= 1 || sh_.ifs().empty());
        char op = dfrest[0];
        bool colon = false;
        size_t opos = 1;
        if (op == ':') { colon = true; op = dfrest[1]; opos = 2; }
        std::string word = dfrest.substr(opos);
        bool empty_test = colon ? joined_empty : unset;

        // Emit the array values exactly as the bare ${a[@]}/${a[*]} path does.
        auto emit_values = [&]() {
          if (dfsel == '@' && dq) {
            absorb_qnull();
            for (size_t k = 0; k < vals.size(); k++) {
              if (k) { out += FIELD_SEP; mask += MMARK; }
              out += QNULL; mask += MMARK;
              for (char c : vals[k]) { out += c; mask += '1'; }
            }
          } else if (dfsel == '*' && dq) {
            std::string is = sh_.ifs();
            std::string j = mb_first_char(is);
            for (size_t k = 0; k < vals.size(); k++) {
              if (k) for (char c : j) { out += c; mask += '1'; }
              for (char c : vals[k]) { out += c; mask += '1'; }
            }
          } else if (dfsel == '*') {
            std::string is = sh_.ifs();
            std::string j = mb_first_char(is);
            for (size_t k = 0; k < vals.size(); k++) {
              if (k) for (char c : j) { out += c; mask += '0'; }
              for (char c : vals[k]) { out += c; mask += '0'; }
            }
          } else {  // unquoted ${a[@]:-w}: drop empties only when splitting
            bool first = true;
            for (size_t k = 0; k < vals.size(); k++) {
              if (splitting_ && vals[k].empty()) continue;
              if (!first) { out += FIELD_SEP; mask += MMARK; }
              first = false;
              for (char c : vals[k]) { out += c; mask += '0'; }
            }
          }
        };
        // Emit the substituted word (subject to the enclosing quoting).
        auto emit_word = [&]() {
          bool has_at = word.find("$@") != std::string::npos ||
                        word.find("[@]") != std::string::npos;
          std::string w = (dq && !sh_.is_zsh() && !has_at) ? expand_dq_word(word)
                                                           : expand_no_split(word);
          char m = dq ? '1' : '0';
          for (char c : w) { out += c; mask += m; }
        };

        std::string dispname = dfname + "[" + std::string(1, dfsel) + "]";
        if (op == '=') {
          // For an ASSOCIATIVE array, `@'/`*' are ordinary literal keys, so
          // `${A[@]:=w}' assigns w to the key `@' when the array is null and then
          // expands the value(s).  For an INDEXED array, bash rejects assignment
          // to the whole array via [@]/[*].
          auto vit = sh_.vars.find(sh_.deref(dfname));
          bool assoc = vit != sh_.vars.end() && vit->second.kind == VarKind::Assoc;
          if (assoc) {
            if (empty_test) {
              sh_.array_set(dfname, std::string(1, dfsel), expand_no_split(word));
              vals = sh_.array_values(dfname);
            }
            emit_values();
          } else {
            std::fprintf(stderr, "%s%s: bad array subscript\n", sh_.err_prefix().c_str(),
                         dispname.c_str());
            sh_.exiting = true;
            sh_.exit_status = 1;
          }
        } else if (op == '-') {
          if (empty_test) emit_word(); else emit_values();
        } else if (op == '+') {
          if (!empty_test) emit_word();  // otherwise substitute nothing
        } else {  // '?': error when the array is unset/null
          if (empty_test) {
            std::string msg = expand_no_split(word);
            if (msg.empty()) msg = colon ? "parameter null or not set" : "parameter not set";
            std::fprintf(stderr, "%s%s: %s\n", sh_.err_prefix().c_str(), dispname.c_str(),
                         msg.c_str());
            sh_.exiting = true;
            sh_.exit_status = 127;
          } else {
            emit_values();
          }
        }
        i = end + 1;
        return;
      }
      // ${a[@]:off:len} / ${@:off:len}: array/positional slice.
      std::string slname, soffx, slenx; char ssel; bool shaslen = false;
      if (slice_ref(body, slname, ssel, soffx, slenx, shaslen)) {
        bool ok = true;
        long long off = eval_arith(sh_, expand_no_split(soffx, false, false), &ok);
        if (!ok) off = 0;
        std::vector<std::string> slice;
        // For an indexed array, `offset' is an array-index threshold: bash takes
        // up to `length' set elements whose index is >= offset (a negative
        // offset counts back from the highest index).  Positional parameters and
        // associative arrays slice by position in their value list instead.
        auto vit = slname.empty() ? sh_.vars.end() : sh_.vars.find(sh_.deref(slname));
        bool indexed = vit != sh_.vars.end() && vit->second.kind == VarKind::Indexed;
        if (indexed) {
          std::vector<std::string> keys = sh_.array_keys(slname);
          std::vector<std::string> vals = sh_.array_values(slname);
          long long maxidx = keys.empty() ? -1 : std::strtoll(keys.back().c_str(), nullptr, 10);
          if (off < 0) { off += maxidx + 1; }
          long long count;
          if (shaslen) {
            long long len = eval_arith(sh_, expand_no_split(slenx, false, false), &ok);
            if (!ok) len = 0;
            count = (len < 0) ? (maxidx + 1 + len - off) : len;
          } else {
            count = static_cast<long long>(keys.size());
          }
          for (size_t k = 0; k < keys.size() &&
                             static_cast<long long>(slice.size()) < count; k++) {
            long long ix = std::strtoll(keys[k].c_str(), nullptr, 10);
            if (ix >= off) slice.push_back(vals[k]);
          }
        } else {
          std::vector<std::string> list;
          if (slname.empty()) {  // positionals: index 0 is $0
            list.push_back(sh_.arg0);
            for (const auto &pp : sh_.positional) list.push_back(pp);
          } else {
            list = sh_.array_values(slname);
          }
          long long n = static_cast<long long>(list.size());
          if (off < 0) { off += n; if (off < 0) off = 0; }
          long long count;
          if (shaslen) {
            long long len = eval_arith(sh_, expand_no_split(slenx, false, false), &ok);
            if (!ok) len = 0;
            count = (len < 0) ? (n + len - off) : len;  // negative len = offset from end
          } else {
            count = n - off;
          }
          for (long long k = off; k < n && static_cast<long long>(slice.size()) < count; k++)
            if (k >= 0) slice.push_back(list[static_cast<size_t>(k)]);
        }
        if (ssel == '*' && dq) {
          std::string is = sh_.ifs();
          std::string j = mb_first_char(is);
          if (!slice.empty()) { out += QNULL; mask += MMARK; }  // "" stays a field
          for (size_t k = 0; k < slice.size(); k++) {
            if (k) for (char c : j) { out += c; mask += '1'; }
            for (char c : slice[k]) { out += c; mask += '1'; }
          }
        } else if (ssel == '*' && splitting_ && sh_.ifs().empty()) {
          // Unquoted ${a[*]:off} with an EMPTY IFS: separate fields, empties
          // dropped (see the plain ${a[*]} case).
          bool first = true;
          for (size_t k = 0; k < slice.size(); k++) {
            if (slice[k].empty()) continue;
            if (!first) { out += FIELD_SEP; mask += MMARK; }
            first = false;
            for (char c : slice[k]) { out += c; mask += '0'; }
          }
        } else if (ssel == '*') {
          // Unquoted (non-empty IFS) / assignment ${a[*]:off}: join with IFS[0],
          // left splittable, so a splitting caller still word-splits on it and an
          // assignment RHS keeps it as the join character.
          std::string is = sh_.ifs();
          std::string j = mb_first_char(is);
          for (size_t k = 0; k < slice.size(); k++) {
            if (k) for (char c : j) { out += c; mask += '0'; }
            for (char c : slice[k]) { out += c; mask += '0'; }
          }
        } else {  // ssel == '@'
          char m = dq ? '1' : '0';
          if (dq) absorb_qnull();
          bool first = true;
          for (size_t k = 0; k < slice.size(); k++) {
            if (!dq && splitting_ && slice[k].empty()) continue;  // vanish when splitting
            if (!first) { out += FIELD_SEP; mask += MMARK; }
            first = false;
            if (dq) { out += QNULL; mask += MMARK; }  // keep empty element
            for (char c : slice[k]) { out += c; mask += m; }
          }
        }
        i = end + 1;
        return;
      }
      op_fields_ = false;  // a pending splice, if any, belongs to THIS body
      std::string val = expand_brace_body(*this, sh_, body, dq);
      if (op_fields_) {
        // A ${x-word}/${x+word} substitute word kept its field structure: splice
        // its (out, mask) verbatim so hard boundaries and quoting are preserved
        // (and data bytes equal to FIELD_SEP/QNULL are not mistaken for markers).
        out += op_out_;
        mask += op_mask_;
        op_fields_ = false;
        op_out_.clear();
        op_mask_.clear();
      } else {
        for (char c : val) { out += c; mask += qm; }
      }
      i = end + 1;
      return;
    }
  }
  // $name or special single char
  std::string name;
  if (std::isalpha(static_cast<unsigned char>(n1)) || n1 == '_') {
    size_t j = i + 1;
    while (j < t.size() && (std::isalnum(static_cast<unsigned char>(t[j])) || t[j] == '_')) j++;
    name = t.substr(i + 1, j - (i + 1));
    i = j;
  } else if (n1 == '@' || n1 == '*') {
    // positional list
    const auto &pos = sh_.positional;
    if (n1 == '@' && dq) {
      absorb_qnull();
      for (size_t k = 0; k < pos.size(); k++) {
        if (k) { out += FIELD_SEP; mask += MMARK; }
        out += QNULL; mask += MMARK;  // keep an empty positional as a field
        for (char c : pos[k]) { out += c; mask += '1'; }
      }
    } else if (n1 == '*' && dq) {
      std::string sep = sh_.ifs();
      std::string joiner = mb_first_char(sep);
      for (size_t k = 0; k < pos.size(); k++) {
        if (k) for (char c : joiner) { out += c; mask += '1'; }
        for (char c : pos[k]) { out += c; mask += '1'; }
      }
    } else if (n1 == '*' && splitting_ && sh_.ifs().empty()) {
      // Unquoted $* with an EMPTY IFS: there is no join character and no field
      // splitting, yet bash still yields the positional parameters as separate
      // fields (empty ones dropped, as an unquoted expansion does).  The usual
      // IFS[0]-join-then-split form below would instead collapse them into one
      // field.  With a non-empty IFS the join form is correct (the join char is
      // also a split char), so it is only overridden here.
      bool first = true;
      for (size_t k = 0; k < pos.size(); k++) {
        if (pos[k].empty()) continue;
        if (!first) { out += FIELD_SEP; mask += MMARK; }
        first = false;
        for (char c : pos[k]) { out += c; mask += '0'; }
      }
    } else if (n1 == '*') {  // assignment / no-split / non-empty-IFS $*: join IFS[0]
      std::string sep = sh_.ifs();
      std::string joiner = mb_first_char(sep);
      for (size_t k = 0; k < pos.size(); k++) {
        if (k) for (char c : joiner) { out += c; mask += '0'; }
        for (char c : pos[k]) { out += c; mask += '0'; }
      }
    } else {  // unquoted $@: empties vanish only when splitting
      bool first = true;
      for (size_t k = 0; k < pos.size(); k++) {
        if (splitting_ && pos[k].empty()) continue;
        if (!first) { out += FIELD_SEP; mask += MMARK; }
        first = false;
        for (char c : pos[k]) { out += c; mask += '0'; }
      }
    }
    i += 2;
    return;
  } else if (n1 == '#' && sh_.is_zsh() && i + 2 < t.size() &&
             (std::isalpha(static_cast<unsigned char>(t[i + 2])) || t[i + 2] == '_')) {
    // zsh `$#name': element count for an array, string length for a scalar.
    size_t j = i + 2;
    while (j < t.size() && (std::isalnum(static_cast<unsigned char>(t[j])) || t[j] == '_')) j++;
    std::string nm = t.substr(i + 2, j - (i + 2));
    std::string cnt = sh_.is_array(nm) ? std::to_string(sh_.array_count(nm))
                                       : std::to_string(sh_.get(nm).size());
    for (char c : cnt) { out += c; mask += qm; }
    i = j;
    return;
  } else if (n1 == '?' || n1 == '$' || n1 == '!' || n1 == '#' || n1 == '-') {
    name = std::string(1, n1);
    i += 2;
  } else if (std::isdigit(static_cast<unsigned char>(n1))) {
    name = std::string(1, n1);
    i += 2;
  } else {
    out += '$';
    mask += qm;
    i += 1;
    return;
  }
  // zsh brace-free subscript: `$name[i]' / `$name[lo,hi]' (1-based).  On an
  // array this indexes elements; on a scalar it indexes characters (substring).
  if (sh_.is_zsh() && !name.empty() &&
      (std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_') &&
      i < t.size() && t[i] == '[' && (sh_.is_array(name) || sh_.is_set(name))) {
    size_t s = i + 1, p = i + 1;
    int d = 1;
    while (p < t.size() && d) { if (t[p] == '[') d++; else if (t[p] == ']') d--; if (d) p++; }
    std::string zsub = t.substr(s, p - s);
    if (p < t.size() && t[p] == ']') p++;
    emit_zsh_subscript(name, zsub, dq, out, mask);
    i = p;
    return;
  }
  // zsh: a bare `$array' expands to every element, not just element 0.  Unquoted
  // it yields one word per element (like `${array[@]}'); double-quoted it joins
  // the elements with the first IFS character (like `"${array[*]}"').
  if (sh_.is_zsh() && sh_.is_array(name)) {
    std::vector<std::string> items = sh_.array_values(name);
    if (dq) {
      std::string is = sh_.ifs();
      std::string joiner = mb_first_char(is);
      for (size_t k = 0; k < items.size(); k++) {
        if (k) for (char c : joiner) { out += c; mask += '1'; }
        for (char c : items[k]) { out += c; mask += '1'; }
      }
    } else {
      for (size_t k = 0; k < items.size(); k++) {
        if (k) { out += FIELD_SEP; mask += MMARK; }
        for (char c : items[k]) { out += c; mask += '0'; }
      }
    }
    return;
  }
  // A bare `$ref' where ref is a nameref to a whole-array splat (`arr[@]'/
  // `arr[*]') expands to every element, like $arr[@] would.
  {
    std::string nrbase;
    char nrsel;
    if (nameref_array_splat(sh_, name, nrbase, nrsel)) {
      emit_array_items(sh_.array_values(nrbase), nrsel, dq, out, mask);
      return;
    }
  }
  bool set = false;
  std::string v = param_value(name, set);
  for (char c : v) { out += c; mask += qm; }
}

// Parse and apply ${...} operators.
// ${var@Q}: single-quote the value so it can be re-read as shell input, using
// $'...' when it contains control characters.
static std::string atq_quote(const std::string &s) {
  bool ctrl = false;
  for (unsigned char c : s)
    if (c < 32 || c == 127) { ctrl = true; break; }
  if (ctrl) {
    std::string r = "$'";
    for (unsigned char c : s) {
      switch (c) {
        case '\n': r += "\\n"; break;
        case '\t': r += "\\t"; break;
        case '\r': r += "\\r"; break;
        case '\\': r += "\\\\"; break;
        case '\'': r += "\\'"; break;
        default:
          if (c < 32 || c == 127) { char b[8]; std::snprintf(b, sizeof b, "\\%03o", c); r += b; }
          else r += static_cast<char>(c);
      }
    }
    return r + "'";
  }
  std::string r = "'";
  for (char c : s) {
    if (c == '\'') r += "'\\''";
    else r += c;
  }
  return r + "'";
}

// True if S contains any non-printable byte (bash's ansic_shouldquote).
static bool kv_has_nonprint(const std::string &s) {
  for (unsigned char c : s)
    if (c < 32 || c == 127) return true;
  return false;
}

// Produce a $'...' ANSI-C quotation of S (used when a kvpair key/value holds
// non-printable bytes).
static std::string kv_ansic_quote(const std::string &s) {
  std::string r = "$'";
  for (unsigned char c : s) {
    switch (c) {
      case '\n': r += "\\n"; break;
      case '\t': r += "\\t"; break;
      case '\r': r += "\\r"; break;
      case '\\': r += "\\\\"; break;
      case '\'': r += "\\'"; break;
      default:
        if (c < 32 || c == 127) { char b[8]; std::snprintf(b, sizeof b, "\\%03o", c); r += b; }
        else r += static_cast<char>(c);
    }
  }
  return r + "'";
}

// Double-quote S, backslash-escaping the characters special inside "..."
// (bash's sh_double_quote).
static std::string kv_double_quote(const std::string &s) {
  std::string r = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\' || c == '$' || c == '`') r += '\\';
    r += c;
  }
  return r + "\"";
}

// True if KEY contains a shell metacharacter that forces quoting in an assoc
// kvpair key (bash's sh_contains_shell_metas).
static bool kv_contains_metas(const std::string &s) {
  for (size_t i = 0; i < s.size(); i++) {
    char c = s[i];
    switch (c) {
      case ' ': case '\t': case '\n': case '\'': case '"': case '\\':
      case '|': case '&': case ';': case '(': case ')': case '<': case '>':
      case '!': case '{': case '}': case '*': case '[': case '?': case ']':
      case '^': case '$': case '`':
        return true;
      case '~':
        if (i == 0 || s[i - 1] == '=' || s[i - 1] == ':') return true;
        break;
      case '#':
        if (i == 0) return true;
        break;
    }
  }
  return false;
}

// Quote a kvpair value: $'...' if it holds non-printables, else "...".
static std::string kv_value_quote(const std::string &v) {
  if (v.empty()) return "\"\"";
  return kv_has_nonprint(v) ? kv_ansic_quote(v) : kv_double_quote(v);
}

// Quote an assoc kvpair key: bare unless it needs $'...'/double-quoting.
static std::string kv_key_quote(const std::string &k) {
  if (kv_has_nonprint(k)) return kv_ansic_quote(k);
  if (kv_contains_metas(k)) return kv_double_quote(k);
  if (k.size() == 1 && (k[0] == '@' || k[0] == '*')) return kv_double_quote(k);
  return k;
}

// Build the ${a[@]@K} single-word "key value" string for array NAME.  Indexed
// keys are bare integers with pairs space-separated; assoc keys are quoted when
// needed and each pair carries a trailing space (matching array/assoc.c).
static std::string kv_build_K(Shell &sh, const std::string &name, bool assoc) {
  auto keys = sh.array_keys(name);
  auto vals = sh.array_values(name);
  std::string r;
  size_t n = std::min(keys.size(), vals.size());
  for (size_t i = 0; i < n; i++) {
    r += assoc ? kv_key_quote(keys[i]) : keys[i];
    r += ' ';
    r += kv_value_quote(vals[i]);
    if (assoc) r += ' ';
    else if (i + 1 < n) r += ' ';
  }
  return r;
}

// ${var@E}: interpret ANSI-C backslash escapes in the value.
static std::string ansic_expand(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] != '\\' || i + 1 >= s.size()) { out += s[i]; continue; }
    switch (s[++i]) {
      case 'n': out += '\n'; break;
      case 't': out += '\t'; break;
      case 'r': out += '\r'; break;
      case 'a': out += '\a'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'v': out += '\v'; break;
      case '\\': out += '\\'; break;
      case '\'': out += '\''; break;
      case '"': out += '"'; break;
      case 'e': out += '\033'; break;
      default: out += '\\'; out += s[i]; break;
    }
  }
  return out;
}

static std::string expand_brace_body(Expander &ex, Shell &sh, const std::string &body,
                                    bool dq) {
  // Leading `#' means length-of.
  bool length = false;
  std::string b = body;
  if (b.size() > 1 && b[0] == '#') {
    // `${#X}' is the LENGTH of parameter X only when X really is a parameter
    // reference; otherwise the `#' is the parameter and the rest an operator
    // (`${#/a/b}' substitutes on the argument count, `${#@Q}' quotes it).
    const std::string r = b.substr(1);
    bool param_ref = r == "@" || r == "*" || r == "#" || r == "?" || r == "-" ||
                     r == "$" || r == "!";
    if (!param_ref && !r.empty()) {
      size_t k = 0;
      if (std::isdigit(static_cast<unsigned char>(r[0]))) {
        while (k < r.size() && std::isdigit(static_cast<unsigned char>(r[k]))) k++;
        param_ref = k == r.size();
      } else if (std::isalpha(static_cast<unsigned char>(r[0])) || r[0] == '_') {
        while (k < r.size() && (std::isalnum(static_cast<unsigned char>(r[k])) || r[k] == '_')) k++;
        param_ref = k == r.size() ||
                    (r[k] == '[' && !r.empty() && r.back() == ']');  // name[sub]
      }
    }
    if (param_ref) { length = true; b = r; }
    else if (std::isdigit(static_cast<unsigned char>(r[0]))) {
      // `${#1xyz}': a positional reference that is not a digit run is not a
      // parameter at all, and `#' takes no such operator (more-exp.tests).
      std::fprintf(stderr, "%s${%s}: bad substitution\n", sh.err_prefix().c_str(),
                   body.c_str());
      sh.arith_error = true;
      return std::string();
    }
  }
  // ${#@} and ${#*} are the count of positional parameters (like $#), not the
  // character length of the expanded list.
  if (length && (b == "@" || b == "*")) return std::to_string(sh.positional.size());
  // An operator with no operand after a SPECIAL parameter is incomplete: a
  // lone `:' never forms one (`${@:}', `${$:}', `${#:}'), and the argument
  // count rejects the other bare operators too (`${#/}', `${#%}', `${#=}',
  // `${#^}', `${#,}') -- with an operand they are fine (more-exp.tests).
  if (!sh.is_zsh() && !length && b.size() == 2 &&
      !(std::isalnum(static_cast<unsigned char>(b[0])) || b[0] == '_') &&
      (b[1] == ':' || (b[0] == '#' && std::strchr("/%=^,+", b[1]) != nullptr))) {
    std::fprintf(stderr, "%s${%s}: bad substitution\n", sh.err_prefix().c_str(),
                 body.c_str());
    sh.arith_error = true;
    return std::string();
  }

  // ${!name} indirection and ${!prefix*}/${!prefix@} name listing.  A `['
  // after the name is the ${!arr[@]} keys form, handled by the array path.
  if (b.size() > 1 && b[0] == '!' &&
      (std::isalpha(static_cast<unsigned char>(b[1])) || b[1] == '_' ||
       b[1] == '#' || std::isdigit(static_cast<unsigned char>(b[1])))) {
    // The indirected parameter name is an identifier, the count `#', or a
    // positional digit run (`${!#}' / `${!2}' indirect through $# / $2).
    size_t q = 1;
    std::string iname;
    if (b[1] == '#') {
      q = 2;
      iname = "#";
    } else if (std::isdigit(static_cast<unsigned char>(b[1]))) {
      while (q < b.size() && std::isdigit(static_cast<unsigned char>(b[q]))) q++;
      iname = b.substr(1, q - 1);
    } else {
      while (q < b.size() && (std::isalnum(static_cast<unsigned char>(b[q])) || b[q] == '_')) q++;
      iname = b.substr(1, q - 1);
    }
    if (q == b.size() || b[q] != '[') {
      if (q + 1 == b.size() && (b[q] == '*' || b[q] == '@')) {
        std::string names;
        for (const auto &kv : sh.vars) {
          if (kv.first.compare(0, iname.size(), iname) != 0) continue;
          if (!names.empty()) names += ' ';
          names += kv.first;
        }
        return names;
      }
      // ${!ref} where ref is a nameref expands to the NAME of the referenced
      // variable, not a second level of indirection (bash special-cases a
      // nameref here).
      auto nit = sh.vars.find(iname);
      if (q == b.size() && nit != sh.vars.end() && nit->second.nameref) {
        std::string tname = sh.deref(iname);
        return length ? std::to_string(mb_charlen(tname)) : tname;
      }
      // The value of INAME is the parameter to expand; any operator that
      // follows applies to the indirected parameter.  A `#'/digit name is a
      // special/positional parameter that sh.get does not resolve, so it goes
      // through the parameter machinery instead of the plain variable lookup.
      std::string target;
      if (iname == "#" || (!iname.empty() && std::isdigit(static_cast<unsigned char>(iname[0])))) {
        bool tset = false;
        target = ex.param_value(iname, tset, false);
      } else {
        target = sh.get(iname);
      }
      // The indirection variable's value is the name to expand; if it is empty
      // (INAME unset or set to the empty string) there is no name to reference,
      // so bash reports `INAME: invalid indirect expansion' and aborts the
      // command.  This fires even with a defaulting operator (`${!x-word}'): the
      // operator applies to the INDIRECTED parameter, not to the missing name.
      if (target.empty()) {
        std::fprintf(stderr, "%s%s: invalid indirect expansion\n", sh.err_prefix().c_str(),
                     iname.c_str());
        sh.arith_error = true;
        return std::string();
      }
      if (length) return std::to_string(mb_charlen(expand_brace_body(ex, sh, target + b.substr(q), dq)));
      return expand_brace_body(ex, sh, target + b.substr(q), dq);
    }
  }

  size_t p = 0;
  std::string name;
  bool named_by_quote = false;
  // extquote: a $'...' in parameter-NAME position decodes to the name
  // (`${$'x1'%$'t'}' is `${x1%t}' -- posixexp7.sub); a BARE-quoted name is
  // bash's `bad substitution' error, and the command aborts.
  if (b.size() >= 3 && b[0] == '$' && b[1] == '\'') {
    size_t qe = b.find('\'', 2);
    if (qe != std::string::npos) {
      name = ansi_c(b.substr(2, qe - 2));
      p = qe + 1;
      named_by_quote = true;
    }
  } else if (!b.empty() && (b[0] == '\'' || b[0] == '"')) {
    // bash's diagnostic shows the body with $'...' forms DEQUOTED to plain
    // quotes (`${'x1'%$'t'}' reports as `${'x1'%'t'}').
    std::string mb;
    for (size_t k = 0; k < b.size(); k++) {
      if (b[k] == '$' && k + 1 < b.size() && b[k + 1] == '\'') continue;
      mb += b[k];
    }
    std::fprintf(stderr, "%s${%s}: bad substitution\n", sh.err_prefix().c_str(), mb.c_str());
    sh.arith_error = true;
    return std::string();
  }
  if (named_by_quote) {
    // fall through to the subscript/operator handling below with NAME set
  } else if (p < b.size() && (b[p] == '@' || b[p] == '*' || b[p] == '?' || b[p] == '$' ||
                       b[p] == '!' || b[p] == '#' || b[p] == '-')) {
    name = b.substr(0, 1);
    p = 1;
  } else if (p < b.size() && (std::isalpha(static_cast<unsigned char>(b[p])) || b[p] == '_' ||
                              std::isdigit(static_cast<unsigned char>(b[p])))) {
    size_t s = p;
    if (std::isdigit(static_cast<unsigned char>(b[p]))) {
      while (p < b.size() && std::isdigit(static_cast<unsigned char>(b[p]))) p++;
    } else {
      while (p < b.size() && (std::isalnum(static_cast<unsigned char>(b[p])) || b[p] == '_')) p++;
    }
    name = b.substr(s, p - s);
  }

  // Optional array subscript name[sub].
  bool have_sub = false;
  std::string sub;
  if (p < b.size() && b[p] == '[') {
    size_t close = skip_subscript(b, p);
    if (close != std::string::npos) {
      sub = b.substr(p + 1, close - p - 1);
      p = close + 1;
    } else {  // unterminated: treat the rest as the subscript
      sub = b.substr(p + 1);
      p = b.size();
    }
    have_sub = true;
  }

  // Text left after a `name[sub]' reference must be an operator: bash rejects
  // `${a[0]junk}' and `${a[0] + b[y]}' as `bad substitution' rather than
  // silently reading the element and dropping the rest (issue #459).
  if (have_sub && p < b.size() && !sh.is_zsh() &&
      !std::strchr(":-+=?#%/^,@", b[p])) {
    std::fprintf(stderr, "%s${%s}: bad substitution\n", sh.err_prefix().c_str(), b.c_str());
    sh.arith_error = true;
    return std::string();
  }

  // zsh: `${#a}' on an array is the element count (bash gives the length of
  // element 0); `${#a[i]}' keeps its meaning (length of that element).
  if (length && !have_sub && sh.is_zsh() && sh.is_array(name))
    return std::to_string(sh.array_count(name));

  // A defaulting/alternative/error operator (`-` `:-` `=` `:=` `+` `:+` `?` `:?`)
  // handles an unset variable itself, so it must not trip `set -u' in
  // param_value.  A bare `:' here begins a substring, not such an operator.
  std::string rest = length ? std::string() : b.substr(p);
  bool defaulting_op = false;
  if (!rest.empty()) {
    char c0 = rest[0];
    if (c0 == '-' || c0 == '=' || c0 == '+' || c0 == '?') defaulting_op = true;
    else if (c0 == ':' && rest.size() > 1 &&
             (rest[1] == '-' || rest[1] == '=' || rest[1] == '+' || rest[1] == '?'))
      defaulting_op = true;
  }

  bool set = false;
  std::string val;
  std::string tsub;  // the expanded, zsh-translated subscript, for have_sub
  if (have_sub) {
    auto vit = sh.vars.find(sh.deref(name));
    bool assoc_sub = vit != sh.vars.end() && vit->second.kind == VarKind::Assoc;
    // An indexed/scalar subscript is an arithmetic expression, which bash expands
    // in a double-quoted context (Q_DOUBLE_QUOTES): single quotes stay literal and
    // a bare `~' is not tilde-expanded, so `${a[' ']}' / `${b[~]}' reach the
    // evaluator as `' '` / `~' and raise a syntax error rather than reading index 0.
    // Associative keys keep the plain expansion (their quoting rules differ).
    std::string esub = assoc_sub ? ex.expand_no_split(sub) : ex.expand_dq_word(sub);
    // An associative subscript that expands empty is an error naming the RAW
    // text: `${#wheat[$unset]}' -> `[$unset]: bad array subscript', and the
    // command aborts (bash).
    if (assoc_sub && esub.empty() && sub != "@" && sub != "*") {
      std::fprintf(stderr, "%s[%s]: bad array subscript\n", sh.err_prefix().c_str(),
                   sub.c_str());
      sh.arith_error = true;
      return std::string();
    }
    if (!sh.array_expand_once_ok(name, esub)) { sh.arith_error = true; return std::string(); }
    // zsh subscripts are 1-based; translate before the (0-based) array read.
    tsub = sh.zsh_subscript(name, esub);
    // An arithmetic (indexed) subscript may have side effects, e.g. ${a[i++]};
    // evaluate it exactly once here and reuse the canonical index for both the
    // read and the element-set test below, so array_get/array_elem_set don't
    // evaluate it (and re-run the side effect) a second time.  A syntax error in
    // the subscript prints bash's arithmetic diagnostic and aborts the command.
    // Associative keys are literal, so leave them untouched.
    if (!assoc_sub && tsub != "@" && tsub != "*") {
      bool aok = true;
      long long idx = eval_arith_msg(sh, tsub, "", &aok);
      if (!aok) { sh.arith_error = true; return std::string(); }
      // A negative index resolves against the highest set index; one still
      // below zero is a bad subscript that aborts the command.  bash's length
      // form names the bracketed raw text (`[-10]: bad array subscript'), the
      // value form the base name (`c: bad array subscript').
      if (idx < 0 && !sh.is_zsh() && vit != sh.vars.end()) {
        // A wholly UNSET variable never errors here: bash's ${#unset[-10]}
        // is 0 and ${unset[-10]} empty (posixexp coverage); only an existing
        // variable with too few elements reports.
        long long maxi = -1;
        if (vit->second.kind == VarKind::Indexed && !vit->second.idx.empty())
          maxi = vit->second.idx.rbegin()->first;
        else if (vit->second.kind == VarKind::Scalar && !vit->second.value.empty())
          maxi = 0;
        idx += maxi + 1;
        if (idx < 0) {
          // The LENGTH form names the bracketed raw text and aborts the
          // command; the VALUE form names the base and expands to empty with
          // the command still running (`echo ${c[-4]}' prints a blank line).
          if (length) {
            std::fprintf(stderr, "%s[%s]: bad array subscript\n", sh.err_prefix().c_str(),
                         sub.c_str());
            sh.arith_error = true;
          } else {
            std::fprintf(stderr, "%s%s: bad array subscript\n", sh.err_prefix().c_str(),
                         name.c_str());
          }
          return std::string();
        }
      }
      tsub = std::to_string(idx);
    }
    val = sh.array_get(name, tsub);
    // A defaulting/alternative operator on a single element (${a[i]-x}, ${a[i]=x},
    // ${a[i]+x}, ${a[i]?}) keys off whether THAT element is set, not the array.
    set = sh.array_elem_set(name, tsub);
    // `set -u': an unset ELEMENT is an unbound-variable error naming the
    // element (`narray[4]: unbound variable'), like param_value's for scalars.
    if (!set && sh.opt_nounset && !defaulting_op && tsub != "@" && tsub != "*") {
      std::fprintf(stderr, "%s%s[%s]: unbound variable\n", sh.err_prefix().c_str(),
                   name.c_str(), tsub.c_str());
      sh.exiting = true;
      sh.exit_status = 127;
      return std::string();
    }
  } else {
    val = ex.param_value(name, set, defaulting_op);
  }
  if (length) return std::to_string(mb_charlen(val));
  return apply_param_op(ex, sh, name, val, set, rest, dq, have_sub, tsub, /*top_level=*/true);
}

// Apply the operator suffix `rest' (everything after the name/subscript) of a
// ${...} expansion to a single value.  Factored out of expand_brace_body so
// array expansions can apply it to each element of ${a[@]} / ${a[*]}.
static std::string apply_param_op(Expander &ex, Shell &sh, const std::string &name,
                                  std::string val, bool set, const std::string &rest,
                                  bool dq, bool have_sub, const std::string &sub,
                                  bool top_level) {
  if (rest.empty()) return val;

  // In a double-quoted context the alternative word is expanded in double-quote
  // context so backslash escapes and literal quotes behave correctly.  Skipped
  // under the zsh personality (different quoting rules) and for words that
  // contain "$@"/"$*" (which carry field structure the dq path would flatten).
  auto expand_word = [&](const std::string &w) {
    bool has_at = w.find("$@") != std::string::npos || w.find("$*") != std::string::npos;
    if (dq && !sh.is_zsh() && !has_at) return ex.expand_dq_word(w);
    return ex.expand_no_split(w);
  };

  // ${name:-word} etc.
  char op = rest[0];
  bool colon = false;
  size_t opos = 0;
  if (op == ':' && rest.size() > 1 &&
      (rest[1] == '-' || rest[1] == '=' || rest[1] == '+' || rest[1] == '?')) {
    colon = true;
    op = rest[1];
    opos = 2;
  } else if (op == '-' || op == '=' || op == '+' || op == '?') {
    opos = 1;
  } else {
    op = '\0';
  }

  if (op == '-' || op == '=' || op == '+' || op == '?') {
    std::string word = rest.substr(opos);
    bool empty = !set || (colon && val.empty());
    // The substitute word of -/+ keeps $*/$@ field boundaries (expand_op_word);
    // = assigns and ? errors, which flatten the word, so they use expand_word.
    if (op == '-') return empty ? ex.expand_op_word(word, dq, top_level) : val;
    if (op == '+') return empty ? std::string() : ex.expand_op_word(word, dq, top_level);
    if (op == '=') {
      if (empty) {
        std::string w = expand_word(word);
        if (have_sub) {
          // Assign to the array element, not a scalar named `name'.  Mirror a
          // plain a[i]=w: an integer-attributed array evaluates the RHS as
          // arithmetic; array_set applies any -u/-l/-c case folding.  Return the
          // stored value so the expansion reflects those transforms.
          auto it = sh.vars.find(sh.deref(name));
          if (it != sh.vars.end() && it->second.integer) {
            bool ok = true;
            w = std::to_string(eval_arith(sh, w, &ok));
          }
          sh.array_set(name, sub, w);
          return sh.array_get(name, sub);
        }
        // A scalar assignment honors the variable's attributes exactly like a
        // plain `name=w': an integer variable evaluates the word
        // arithmetically and -u/-l/-c fold its case, and the EXPANSION is the
        // stored value (`declare -i a; echo ${a:=4+3}' prints 7 -- exp13.sub).
        auto sit = sh.vars.find(sh.deref(name));
        if (sit != sh.vars.end() && sit->second.integer) {
          bool iok = true;
          w = std::to_string(eval_arith(sh, w, &iok));
        }
        sh.set(name, w);
        std::string stored;
        return sh.get_if_set(name, stored) ? stored : w;
      }
      return val;
    }
    if (op == '?') {
      if (empty) {
        std::string msg = expand_word(word);
        if (msg.empty()) msg = colon ? "parameter null or not set" : "parameter not set";
        std::fprintf(stderr, "%s%s: %s\n", sh.err_prefix().c_str(), name.c_str(), msg.c_str());
        sh.exiting = true;
        sh.exit_status = 127;  // bash: a fatal ${x?} / set -u error exits with 127
        return std::string();
      }
      return val;
    }
  }

  // ${name#pat} ${name##pat} ${name%pat} ${name%%pat}
  if (rest[0] == '#' || rest[0] == '%') {
    bool longest = rest.size() > 1 && rest[1] == rest[0];
    std::string pat = ex.expand_pattern(rest.substr(longest ? 2 : 1));
    // Try only CHARACTER-boundary split points, so `${x%?}' removes one whole
    // multibyte character rather than a single trailing byte (which would leave
    // a broken sequence).  cb lists the byte offsets of every boundary, 0..size.
    std::vector<size_t> cb;
    for (size_t i = 0;; ) {
      cb.push_back(i);
      if (i >= val.size()) break;
      size_t len = 1;
      mb_decode(val, i, len);
      i += len;
    }
    if (rest[0] == '#') {  // prefix removal (shortest = first, longest = last)
      if (longest) {
        for (size_t j = cb.size(); j-- > 0; )
          if (pat_match(pat, val.substr(0, cb[j]))) return val.substr(cb[j]);
      } else {
        for (size_t k : cb)
          if (pat_match(pat, val.substr(0, k))) return val.substr(k);
      }
    } else {  // suffix removal (longest = first split, shortest = last split)
      if (longest) {
        for (size_t k : cb)
          if (pat_match(pat, val.substr(k))) return val.substr(0, k);
      } else {
        for (size_t j = cb.size(); j-- > 0; )
          if (pat_match(pat, val.substr(cb[j]))) return val.substr(0, cb[j]);
      }
    }
    return val;
  }

  // ${name/pat/rep} ${name//pat/rep} ${name/#pat/rep} ${name/%pat/rep}
  if (rest[0] == '/') {
    bool global = rest.size() > 1 && rest[1] == '/';
    std::string body2 = rest.substr(global ? 2 : 1);
    // A leading `#'/`%' anchors the pattern to the start/end of the value.
    char anchor = 0;
    if (!body2.empty() && (body2[0] == '#' || body2[0] == '%')) {
      anchor = body2[0];
      body2 = body2.substr(1);
    }
    size_t slash = std::string::npos;
    for (size_t k = 0; k < body2.size(); k++) {
      if (body2[k] == '\\') { k++; continue; }
      if (body2[k] == '/') { slash = k; break; }
    }
    std::string pat = ex.expand_pattern(slash == std::string::npos ? body2 : body2.substr(0, slash));
    // The replacement undergoes full quote removal like a normal word --
    // independent of whether the enclosing ${...} is double-quoted -- so single
    // quotes, double quotes, and a backslash before any character are all
    // processed (`\'' -> `'', `'ab'' -> `ab'), matching bash.
    std::string rep = slash == std::string::npos
                          ? std::string()
                          : ex.expand_no_split(body2.substr(slash + 1));
    // `#' matches the longest prefix, `%' the longest suffix; one replacement.
    if (anchor == '#') {
      for (size_t j = val.size() + 1; j-- > 0;)
        if (pat_match(pat, val.substr(0, j))) return rep + val.substr(j);
      return val;
    }
    if (anchor == '%') {
      for (size_t j = 0; j <= val.size(); j++)
        if (pat_match(pat, val.substr(j))) return val.substr(0, j) + rep;
      return val;
    }
    if (pat.empty()) return val;
    std::string result;
    size_t k = 0;
    bool did = false;
    while (k < val.size()) {
      size_t best = std::string::npos;
      for (size_t j = val.size(); j > k; j--) {
        if (pat_match(pat, val.substr(k, j - k))) { best = j; break; }
      }
      if (best != std::string::npos && (!did || global)) {
        result += rep;
        k = (best == k) ? k + 1 : best;  // avoid infinite loop on empty match
        did = true;
        if (!global) {
          result += val.substr(k);
          return result;
        }
      } else {
        result += val[k++];
      }
    }
    return result;
  }

  // ${name^} ${name^^} ${name,} ${name,,}  (case modification)
  if (rest[0] == '^' || rest[0] == ',') {
    bool all = rest.size() > 1 && rest[1] == rest[0];
    std::string pat = ex.expand_pattern(rest.substr(all ? 2 : 1));
    bool up = rest[0] == '^';
    return mb_case_fold(val, pat, all, up ? 'U' : 'L',
                        [&](const std::string &cs) { return pat_match(pat, cs); });
  }

  // ${name~} ${name~~}  (case toggle: ~ the first matching char, ~~ all)
  if (rest[0] == '~') {
    bool all = rest.size() > 1 && rest[1] == '~';
    std::string pat = ex.expand_pattern(rest.substr(all ? 2 : 1));
    return mb_case_fold(val, pat, all, 'T',
                        [&](const std::string &cs) { return pat_match(pat, cs); });
  }

  // ${name@op} -- parameter transformations.
  if (rest[0] == '@' && rest.size() >= 2) {
    // @a/@A report the variable's attributes even when its scalar context
    // (element 0) is unset -- an assoc array without ["0"] still has them.
    if (!set && !(rest[1] == 'a' || rest[1] == 'A') )
      return std::string();  // unset -> empty (even for @Q)
    if (!set && sh.vars.find(name) == sh.vars.end()) return std::string();
    char t = rest[1];
    // Scalar/bare-name @k/@K quote the (element-0) value like @Q; the array
    // subscript forms ${a[@]@k}/${a[@]@K} are handled at the array dispatch.
    if (t == 'Q' || t == 'k' || t == 'K') return atq_quote(val);
    if (t == 'E') return ansic_expand(val);
    if (t == 'P') return expand_prompt(sh, val);
    if (t == 'U' || t == 'L' || t == 'u') {
      // @U upper-case all, @L lower-case all, @u upper-case only the first
      // character -- all character-aware.  No pattern (every character matches).
      auto yes = [](const std::string &) { return true; };
      return mb_case_fold(val, "", /*all=*/t != 'u', t == 'L' ? 'L' : 'U', yes);
    }
    if (t == 'a' || t == 'A') {
      auto it = sh.vars.find(name);
      std::string flags;
      bool is_arr = false, is_assoc = false;
      if (it != sh.vars.end()) {
        const Variable &var = it->second;
        is_arr = var.kind == VarKind::Indexed;
        is_assoc = var.kind == VarKind::Assoc;
        if (is_arr) flags += 'a';
        if (is_assoc) flags += 'A';
        if (var.integer) flags += 'i';
        if (var.readonly) flags += 'r';
        if (var.exported) flags += 'x';
      }
      if (t == 'a') return flags;
      // @A: reproduce a declare/assignment statement.
      std::string q = atq_quote(val);
      if (flags.empty()) return name + "=" + q;
      return "declare -" + flags + " " + name + "=" + q;
    }
    return val;
  }

  // ${name:offset:length} -- offset/length undergo parameter expansion before
  // the arithmetic (`${PARAM:$OFFSET}', `${PARAM:${OFFSET:-0}}').
  if (rest[0] == ':') {
    std::string args = rest.substr(1);
    size_t colon2 = length_colon(args);
    bool ok = true;
    // Offset and length are ARITHMETIC contexts: expand them the way (( ))
    // does so a `NAME[...]' subscript is copied raw and expanded exactly once
    // by the evaluator -- keys holding `]'/`['/$(...) then resolve
    // (`${string:A[%]:A[$k3]}' with k3=`]' -- quotearray.tests).
    std::string offtxt = colon2 == std::string::npos ? args : args.substr(0, colon2);
    // bash prefixes a substring offset/length error with the PARAMETER
    // (`${#:%}' -> `#: %: arithmetic syntax error ...').
    long long off =
        eval_arith_msg(sh, ex.expand_arith(offtxt), name.c_str(), &ok, /*expand_subs=*/1);
    long long len = -1;
    if (ok && colon2 != std::string::npos)
      len = eval_arith_msg(sh, ex.expand_arith(args.substr(colon2 + 1)), name.c_str(), &ok,
                           /*expand_subs=*/1);
    // A failed offset/length aborts the command, as any expansion error does.
    if (!ok) {
      sh.arith_error = true;
      return std::string();
    }
    // Offset and length are counted in characters (bash), not bytes: map them
    // through mb_byteoff so a UTF-8 value slices on code-point boundaries.
    long long n = static_cast<long long>(mb_charlen(val));
    if (off < 0) off += n;
    if (off < 0) off = 0;
    if (off > n) off = n;
    std::string res = val.substr(mb_byteoff(val, static_cast<size_t>(off)));
    if (len >= 0 && len < static_cast<long long>(mb_charlen(res)))
      res = res.substr(0, mb_byteoff(res, static_cast<size_t>(len)));
    return res;
  }

  return val;
}

void Expander::process_dq(const std::string &text, size_t &i, std::string &out,
                          std::string &mask) {
  while (i < text.size() && text[i] != '"') {
    if (text[i] == '\\' && i + 1 < text.size() &&
        (text[i + 1] == '$' || text[i + 1] == '`' || text[i + 1] == '"' ||
         text[i + 1] == '\\')) {
      out += text[i + 1];
      mask += '1';
      i += 2;
    } else if (text[i] == '$') {
      expand_dollar(text, i, true, out, mask);
    } else if (text[i] == '`') {
      size_t j = i + 1;
      std::string inner;
      while (j < text.size() && text[j] != '`') {
        if (text[j] == '\\' && j + 1 < text.size() &&
            (text[j + 1] == '`' || text[j + 1] == '\\' || text[j + 1] == '$')) {
          inner += text[j + 1];
          j += 2;
        } else {
          inner += text[j++];
        }
      }
      int st = 0;
      std::string res = sh_.run_and_capture(inner, &st);
      sh_.note_cmdsub(st);
      for (char ch : res) { out += ch; mask += '1'; }
      i = (j < text.size()) ? j + 1 : j;
    } else {
      out += text[i];
      mask += '1';
      i++;
    }
  }
}

// Expand TEXT for an arithmetic context ($((...)), (( )), $[...]): parameter,
// command, and arithmetic expansion as if inside double quotes; unescaped
// double-quote characters are removed but single quotes stay ORDINARY
// characters, so `$(( "1+1" ))' is 2 while `$(( 'foo' ))' is the arithmetic
// syntax error bash reports (expr.c never sees quoting).
// Mark characters of arithmetic-expansion OUTPUT that are special to the
// arithmetic scanner with a \x04 display-escape byte: the scanner treats the
// pair structurally-neutral or strips it, and error messages render it as a
// backslash (`((: 'assoc[x\],b\[\$(echo uname >&2)]++' : ...`, as bash).
static void arith_escape(std::string &out, std::string &mask, size_t from) {
  std::string o, m;
  o.reserve(out.size() - from + 8);
  for (size_t k = from; k < out.size(); k++) {
    char c = out[k];
    if (c == '[' || c == ']' || c == '$' || c == '\\' || c == '`') {
      o += '\x04';
      m += k < mask.size() ? mask[k] : '1';
    }
    o += c;
    m += k < mask.size() ? mask[k] : '1';
  }
  out.resize(from);
  mask.resize(from < mask.size() ? from : mask.size());
  out += o;
  mask += m;
}

std::string Expander::expand_arith(const std::string &text) {
  std::string out, mask;
  size_t i = 0;
  while (i < text.size()) {
    char c = text[i];
    // A `NAME[...]' array subscript is copied RAW: the arithmetic evaluator
    // expands it exactly once itself (bash expands subscripts at evaluation,
    // so `(( assoc[$key]++ ))' with `]'/`$(' in $key's VALUE keys on the
    // literal value and never re-scans or executes it).
    bool sub_ctx = false;
    if (c == '[' && !out.empty() &&
        (std::isalnum(static_cast<unsigned char>(out.back())) || out.back() == '_')) {
      // Walk back over the name: a quote directly before it (`'assoc[$k]++'`)
      // means this is quoted DATA, not an array reference -- expand normally
      // so the error text shows the expansion, escaped.
      size_t nb = out.size();
      while (nb > 0 && (std::isalnum(static_cast<unsigned char>(out[nb - 1])) || out[nb - 1] == '_'))
        nb--;
      sub_ctx = nb == 0 || (out[nb - 1] != '\'' && out[nb - 1] != '"');
    }
    if (sub_ctx) {
      int bd = 0;
      size_t j = i;
      for (; j < text.size(); j++) {
        char b = text[j];
        if (b == '\\' && j + 1 < text.size()) { j++; continue; }
        if (b == '\'') { while (++j < text.size() && text[j] != '\'') {} continue; }
        if (b == '"') { while (++j < text.size() && text[j] != '"') if (text[j] == '\\') j++; continue; }
        if (b == '$' && j + 1 < text.size() && text[j + 1] == '(') {
          size_t e = scan_balanced(text, j + 1, '(', ')');
          if (e != std::string::npos) { j = e; continue; }
        }
        if (b == '[') bd++;
        else if (b == ']' && --bd == 0) break;
      }
      if (j < text.size()) {  // balanced: emit the span untouched
        for (size_t k = i; k <= j; k++) { out += text[k]; mask += '0'; }
        i = j + 1;
        continue;
      }
    }
    if (c == '\\' && i + 1 < text.size() &&
        (text[i + 1] == '$' || text[i + 1] == '`' || text[i + 1] == '"' ||
         text[i + 1] == '\\')) {
      out += text[i + 1];
      mask += '1';
      i += 2;
      continue;
    }
    if (c == '$') {
      size_t o0 = out.size();
      expand_dollar(text, i, true, out, mask);
      arith_escape(out, mask, o0);
      continue;
    }
    if (c == '`') {
      size_t j = i + 1;
      std::string inner;
      while (j < text.size() && text[j] != '`') {
        if (text[j] == '\\' && j + 1 < text.size() &&
            (text[j + 1] == '`' || text[j + 1] == '\\' || text[j + 1] == '$')) {
          inner += text[j + 1];
          j += 2;
        } else {
          inner += text[j++];
        }
      }
      int st = 0;
      std::string res = sh_.run_and_capture(inner, &st);
      sh_.note_cmdsub(st);
      size_t o0 = out.size();
      for (char ch : res) { out += ch; mask += '1'; }
      arith_escape(out, mask, o0);
      i = (j < text.size()) ? j + 1 : j;
      continue;
    }
    if (c == '"') { i++; continue; }
    out += c;
    mask += '0';
    i++;
  }
  // `$@'/`$*' field markers become plain spaces in arithmetic -- `set -- 1 +
  // 2' makes `$(( $@ ))' evaluate `1 + 2' (bash joins the positionals with
  // spaces here, quoted or not) -- and the QNULL empty-field markers vanish.
  std::string flat;
  flat.reserve(out.size());
  for (char ch : out) {
    if (ch == FIELD_SEP) flat += ' ';
    else if (ch != QNULL) flat += ch;
  }
  return flat;
}

std::string Expander::expand_dq_word(const std::string &w_in) {
  // bash recognizes $'...' ANSI-C quoting in the replacement word of
  // ${var:-word} (etc.) even when the whole expansion is double quoted, though
  // ordinary single quotes there stay literal.  Pre-decode each $'...' and
  // escape the resulting bytes so the double-quote pass below keeps them
  // literal.
  std::string w;
  for (size_t k = 0; k < w_in.size(); k++) {
    if (w_in[k] == '\\' && k + 1 < w_in.size()) {
      // `\}' escaped the ${...} closer at scan time: the word keeps a literal
      // `}' and the backslash is consumed (`"${IFS+\}z}"' -> `}z'), unlike
      // the ordinary double-quote escapes which process() handles below.
      if (w_in[k + 1] == '}') { w += '}'; k++; continue; }
      w += w_in[k]; w += w_in[k + 1]; k++; continue;
    }
    if (w_in[k] == '$' && k + 1 < w_in.size() && w_in[k + 1] == '\'') {
      size_t j = k + 2;
      while (j < w_in.size() && w_in[j] != '\'') { if (w_in[j] == '\\' && j + 1 < w_in.size()) j++; j++; }
      std::string decoded = ansi_c(w_in.substr(k + 2, j - (k + 2)));
      for (char c : decoded) {
        if (c == '"' || c == '\\' || c == '$' || c == '`') w += '\\';
        w += c;
      }
      k = j;  // the loop's ++ steps past the closing quote
      continue;
    }
    w += w_in[k];
  }
  // A synthetic leading quote starts the double-quote span; embedded quotes in
  // W toggle context normally (an unterminated span at the end is fine).  A
  // single quote is literal throughout (bash's DOLBRACE_QUOTE state), even when
  // the word's own double quotes toggle the context (sq_literal).
  std::string out, mask;
  bool saved_split = splitting_;
  splitting_ = false;
  process('"' + w, out, mask, false, false, /*sq_literal=*/true);
  splitting_ = saved_split;
  std::string joined;
  for (size_t k = 0; k < out.size(); k++) {
    if (k < mask.size() && mask[k] == MMARK) {
      if (out[k] == FIELD_SEP) joined += ' ';
      continue;
    }
    joined += out[k];
  }
  return joined;
}

void Expander::process(const std::string &text, std::string &out, std::string &mask,
                       bool /*assignment_rhs*/, bool heredoc, bool sq_literal) {
  // Most output is about as long as the input; reserve to avoid reallocating
  // (and memmoving) the out/mask pair as they grow char by char.
  out.reserve(out.size() + text.size());
  mask.reserve(mask.size() + text.size());
  size_t i = 0;
  while (i < text.size()) {
    char c = text[i];
    if (heredoc && (c == '\'' || c == '"')) {
      // Inside a here-document, quote characters are ordinary text.
      out += c; mask += '2'; i++;
    } else if (c == '\'' && sq_literal) {
      // In a double-quoted ${...} operator word a single quote is literal.
      out += c; mask += '1'; i++;
    } else if (c == '\'') {
      out += QNULL; mask += MMARK;  // a quote region yields a field even if empty
      i++;
      while (i < text.size() && text[i] != '\'') { out += text[i]; mask += '1'; i++; }
      if (i < text.size()) i++;
    } else if (c == '"') {
      out += QNULL; mask += MMARK;
      i++;
      process_dq(text, i, out, mask);
      if (i < text.size()) i++;
    } else if (!heredoc && c == '$' && i + 1 < text.size() && text[i + 1] == '"') {
      i++;  // $"...": locale-translated string; treated as a plain "..."
    } else if (!heredoc && c == '$' && i + 1 < text.size() && text[i + 1] == '\'') {
      out += QNULL; mask += MMARK;
      size_t j = i + 2;
      std::string inner;
      while (j < text.size() && text[j] != '\'') {
        if (text[j] == '\\' && j + 1 < text.size()) { inner += text[j]; inner += text[j + 1]; j += 2; }
        else inner += text[j++];
      }
      std::string dec = ansi_c(inner);
      for (char ch : dec) { out += ch; mask += '1'; }
      i = (j < text.size()) ? j + 1 : j;
    } else if (c == '\\') {
      if (heredoc) {
        // In a here-document, a backslash escapes only $, `, \, and newline;
        // before anything else it is a literal character.
        char nx = (i + 1 < text.size()) ? text[i + 1] : '\0';
        if (nx == '$' || nx == '`' || nx == '\\') { out += nx; mask += '2'; i += 2; }
        else if (nx == '\n') { i += 2; }  // line continuation
        else { out += c; mask += '2'; i++; }
      } else if (i + 1 < text.size()) { out += text[i + 1]; mask += '1'; i += 2; }
      else {
        // A backslash with nothing to escape is a literal character (bash:
        // `echo escape\' prints the backslash -- quote.tests).
        out += c;
        mask += '1';
        i++;
      }
    } else if (c == '$') {
      expand_dollar(text, i, false, out, mask, heredoc);
    } else if (c == '`') {
      size_t j = i + 1;
      std::string inner;
      while (j < text.size() && text[j] != '`') {
        if (text[j] == '\\' && j + 1 < text.size() &&
            (text[j + 1] == '`' || text[j + 1] == '\\' || text[j + 1] == '$'))
          { inner += text[j + 1]; j += 2; }
        else
          inner += text[j++];
      }
      int st = 0;
      std::string res = sh_.run_and_capture(inner, &st);
      sh_.note_cmdsub(st);
      for (char ch : res) { out += ch; mask += '4'; }  // command output: zsh-splittable
      i = (j < text.size()) ? j + 1 : j;
    } else {
      // Literal (unquoted, not from expansion): mask '2' -- glob-active like an
      // unquoted char, but NOT subject to IFS word-splitting (only expansion
      // output, mask '0', is split).
      out += c;
      mask += '2';
      i++;
    }
  }
}

std::vector<std::pair<std::string, std::string>> Expander::split_ifs(const std::string &s,
                                                                     const std::string &mask) {
  std::string ifs = sh_.ifs();
  // Mask legend for IFS splitting:
  //   '0' parameter/array/positional expansion output -- IFS-split in bash, but
  //       NOT in zsh (zsh leaves `$var' and array elements un-split);
  //   '4' command-substitution output ($(...)/`...`) -- IFS-split in both bash
  //       and zsh (zsh word-splits command substitution even with the default
  //       SH_WORD_SPLIT off);
  //   '1' quoted, '2' literal -- never IFS-split.
  // FIELD_SEP (array/`$@' element boundary) is always a hard split for any
  // unquoted expansion output, in either shell.
  bool zsh = sh_.is_zsh();
  auto splittable = [&](char m) { return m == '4' || (m == '0' && !zsh); };
  std::vector<std::pair<std::string, std::string>> fields;
  // IFS as a list of (possibly multibyte) characters, so a multibyte separator
  // like `€' splits on the whole character rather than on each of its bytes.
  std::vector<std::string> ifs_chars;
  for (size_t k = 0; k < ifs.size();) {
    size_t len = 1;
    mb_decode(ifs, k, len);
    ifs_chars.push_back(ifs.substr(k, len));
    k += len;
  }
  auto clen = [&](size_t i) { size_t len = 1; mb_decode(s, i, len); return len; };
  auto is_ifs = [&](size_t i) {
    std::string ch = s.substr(i, clen(i));
    for (const std::string &c : ifs_chars)
      if (c == ch) return true;
    return false;
  };
  auto is_ws = [&](char c) { return c == ' ' || c == '\t' || c == '\n'; };
  auto soft_ifs = [&](size_t i) { return splittable(mask[i]) && is_ifs(i); };
  auto soft_ws = [&](size_t i) { return soft_ifs(i) && is_ws(s[i]); };
  // bash's list_string algorithm (lib/sh/split.c): leading IFS whitespace is
  // skipped, then each iteration extracts one field and consumes a single
  // delimiter of the form [IFS ws]* [one IFS non-ws]? [IFS ws]*.  This makes
  // ` :' (whitespace then a non-whitespace IFS char) a single delimiter, so
  // `IFS=": "; set -- $x' on x="a :" yields just "a" rather than "a" plus a
  // spurious empty field.  Quoted ('1') and literal ('2') text is never a
  // delimiter, even when it contains IFS characters.  FIELD_SEP marks a hard
  // array/`$@' element boundary that always splits (empty elements preserved).
  size_t n = s.size(), i = 0;
  while (i < n && soft_ws(i)) i++;  // strip leading IFS whitespace
  while (i < n) {
    std::string cur, curm;
    while (i < n && !soft_ifs(i) && !(mask[i] == MMARK && s[i] == FIELD_SEP)) {
      cur += s[i];
      curm += mask[i];
      i++;
    }
    fields.emplace_back(cur, curm);
    if (i >= n) break;
    if (mask[i] == MMARK && s[i] == FIELD_SEP) {
      i++;  // hard boundary; skip any IFS whitespace leading the next element
      while (i < n && soft_ws(i)) i++;
      continue;
    }
    // Soft IFS delimiter: [ws]* [one non-ws]? [ws]*.  A non-whitespace IFS
    // delimiter may be multibyte, so consume the whole character.
    while (i < n && soft_ws(i)) i++;
    if (i < n && soft_ifs(i) && !is_ws(s[i])) {
      i += clen(i);
      while (i < n && soft_ws(i)) i++;
    }
  }
  return fields;
}

// Whether an unquoted `[' at position i opens a valid bracket expression: there
// must be a later unquoted `]' to close it.  A `]' immediately after `[' (or
// after a leading `!'/`^' negation) is a literal member, not the close.  With no
// closing `]' the `[' is an ordinary character -- bash does not treat such a
// word as a glob, so it needs no directory scan (important for the very common
// `[ ... ]' test command, whose `[' would otherwise trigger a scan per call).
static bool opens_bracket(const std::string &field, const std::string &mask, size_t i) {
  size_t j = i + 1;
  if (j < field.size() && mask[j] != '1' && (field[j] == '!' || field[j] == '^')) j++;
  if (j < field.size() && mask[j] != '1' && field[j] == ']') j++;  // literal first `]'
  for (; j < field.size(); j++) {
    // Posix 2.13.3: an unquoted slash renders the bracket expression invalid,
    // so `[qwe/qwe]' is not a pattern at all (a quoted `/' is fine).
    if (mask[j] != '1' && field[j] == '/') return false;
    if (mask[j] != '1' && field[j] == ']') return true;
  }
  return false;
}

// $GLOBSORT (bash 5.3, pathexp.c setup_globsort): an optional leading `+'
// (ignored) or `-' (reverse), then one of name/size/mtime/atime/ctime/blocks/
// numeric/nosort.  Unset, empty, or an unrecognized keyword mean the default
// name sort in ascending order (an unknown keyword also drops the `-').  Ties
// under the stat-based sorts fall back to the name comparison, which itself
// honors the reverse flag ("secondary sorting preserves reverse ordering").
static void globsort_results(Shell &sh, std::vector<std::string> &v) {
  enum SortType { S_NAME, S_SIZE, S_MTIME, S_ATIME, S_CTIME, S_BLOCKS, S_NUMERIC, S_NOSORT };
  std::string spec = sh.get("GLOBSORT");
  size_t p = 0;
  while (p < spec.size() && (spec[p] == ' ' || spec[p] == '\t')) p++;
  bool rev = false;
  if (p < spec.size() && (spec[p] == '+' || spec[p] == '-')) rev = (spec[p++] == '-');
  std::string t = spec.substr(p);
  SortType type;
  if (spec.empty()) type = S_NAME;                       // unset/empty: default
  else if (t.empty()) type = S_NAME;                     // bare `+' / `-'
  else if (t == "name") type = S_NAME;
  else if (t == "size") type = S_SIZE;
  else if (t == "mtime") type = S_MTIME;
  else if (t == "atime") type = S_ATIME;
  else if (t == "ctime") type = S_CTIME;
  else if (t == "blocks") type = S_BLOCKS;
  else if (t == "numeric") type = S_NUMERIC;
  else if (t == "nosort") type = S_NOSORT;
  else { type = S_NAME; rev = false; }                   // unknown: historical
  if (type == S_NOSORT) return;
  auto namecmp = [rev](const std::string &a, const std::string &b) {
    return rev ? b.compare(a) : a.compare(b);
  };
  if (type == S_NAME) {
    std::sort(v.begin(), v.end(),
              [&](const std::string &a, const std::string &b) { return namecmp(a, b) < 0; });
    return;
  }
  struct Ent {
    std::string name;
    bool statted;
    struct stat st;
  };
  std::vector<Ent> ents(v.size());
  for (size_t i = 0; i < v.size(); i++) {
    ents[i].name = v[i];
    ents[i].statted = stat(v[i].c_str(), &ents[i].st) == 0;
    if (!ents[i].statted) std::memset(&ents[i].st, 0, sizeof(ents[i].st));
  }
#ifdef __APPLE__
#define GN_ST_MTIM st_mtimespec
#define GN_ST_ATIM st_atimespec
#define GN_ST_CTIM st_ctimespec
#else
#define GN_ST_MTIM st_mtim
#define GN_ST_ATIM st_atim
#define GN_ST_CTIM st_ctim
#endif
  auto gencmp = [](long long a, long long b) { return (a > b) - (a < b); };
  auto cmp = [&](const Ent &a, const Ent &b) {
    int x = 0;
    switch (type) {
      case S_SIZE:
        x = gencmp(a.st.st_size, b.st.st_size);
        break;
      case S_BLOCKS:
        x = gencmp(a.st.st_blocks, b.st.st_blocks);
        break;
      case S_MTIME:
      case S_ATIME:
      case S_CTIME: {
        struct timespec ta, tb;
        if (type == S_MTIME) { ta = a.st.GN_ST_MTIM; tb = b.st.GN_ST_MTIM; }
        else if (type == S_ATIME) { ta = a.st.GN_ST_ATIM; tb = b.st.GN_ST_ATIM; }
        else { ta = a.st.GN_ST_CTIM; tb = b.st.GN_ST_CTIM; }
        x = gencmp(ta.tv_sec, tb.tv_sec);
        if (x == 0) x = gencmp(ta.tv_nsec, tb.tv_nsec);
        break;
      }
      case S_NUMERIC: {
        // Names that are all digits compare as numbers; a numeric name sorts
        // before a non-numeric one; two non-numeric names compare as names
        // (and that comparison needs no reverse-tiebreak special case).
        auto num = [](const std::string &s, long long &out) {
          if (s.empty()) return false;
          for (char c : s)
            if (c < '0' || c > '9') return false;
          out = std::strtoll(s.c_str(), nullptr, 10);
          return true;
        };
        long long ia = 0, ib = 0;
        bool va = num(a.name, ia), vb = num(b.name, ib);
        if (va && vb) x = gencmp(ia, ib);
        else if (!va && !vb) return namecmp(a.name, b.name) < 0;
        else x = va ? -1 : 1;
        break;
      }
      default:
        break;
    }
    if (rev) x = -x;
    if (x != 0) return x < 0;
    return namecmp(a.name, b.name) < 0;
  };
  std::sort(ents.begin(), ents.end(), cmp);
  for (size_t i = 0; i < v.size(); i++) v[i] = std::move(ents[i].name);
}

std::vector<std::string> Expander::glob_field(const std::string &field, const std::string &mask) {
  // Build a glob pattern: unquoted metacharacters stay special, quoted ones are
  // backslash-escaped; also produce the literal (quotes already removed).
  std::string pattern;
  bool magic = false;
  // Under `shopt -s extglob', the operators +( !( @( *( ?( also make a word a
  // pattern (the `*('/`?(' forms are already caught by the `*'/`?' below).
  auto eg = sh_.shopt_opts.find("extglob");
  bool extglob = eg != sh_.shopt_opts.end() && eg->second;
  for (size_t i = 0; i < field.size(); i++) {
    char c = field[i];
    bool q = mask[i] == '1';
    // A backslash in unquoted expansion data quotes the following character
    // for the matcher, so neither it nor that character can make the word a
    // pattern (bash unquoted_glob_pattern_p skips both): `var='a\?'; echo
    // ${var}' is not a glob even with a file `a?' present.
    if (!q && c == '\\') {
      pattern += c;
      if (i + 1 < field.size()) {
        char n = field[++i];
        if (mask[i] == '1' && (n == '*' || n == '?' || n == '[' || n == '\\' || n == ']'))
          pattern += '\\';
        pattern += n;
      }
      continue;
    }
    if (!q && (c == '*' || c == '?' ||
               (c == '[' && opens_bracket(field, mask, i))))
      magic = true;
    if (!q && extglob && (c == '+' || c == '!' || c == '@' || c == '*' || c == '?') &&
        i + 1 < field.size() && field[i + 1] == '(')
      magic = true;
    if (q && (c == '*' || c == '?' || c == '[' || c == '\\' || c == ']')) pattern += '\\';
    pattern += c;
  }
  if (sh_.opt_noglob || !magic) return {field};
  int gflags = 0;
  // `**' recursive globbing is on under `shopt -s globstar', and -- because zsh
  // enables it by default -- whenever the personality is zsh or the `zsh_globbing'
  // variable is set to a non-null value.
  auto gs = sh_.shopt_opts.find("globstar");
  bool globstar = (gs != sh_.shopt_opts.end() && gs->second) || sh_.is_zsh() ||
                  !sh_.get("zsh_globbing").empty();
  if (globstar) gflags |= GX_GLOBSTAR;
  // `shopt -s dotglob': a leading `.' is matched by ordinary patterns too
  // (except the `.'/`..' entries, which are always skipped).
  auto dg = sh_.shopt_opts.find("dotglob");
  if (dg != sh_.shopt_opts.end() && dg->second) gflags |= GX_MATCHDOT;
  // `shopt -u globskipdots' lets `.' and `..' be matched.
  auto gsd = sh_.shopt_opts.find("globskipdots");
  if (gsd != sh_.shopt_opts.end() && !gsd->second) gflags |= GX_NODOTSKIP;
  // A non-null $GLOBIGNORE also enables dot matching, then filters out any
  // result matching one of its colon-separated patterns.
  std::string globignore = sh_.get("GLOBIGNORE");
  if (!globignore.empty()) gflags |= GX_MATCHDOT;
  auto matches = gnash::glob::glob(pattern, gflags);
  if (!globignore.empty()) {
    // Split on `:' but not inside a bracket expression -- a `[:class:]' has
    // its own colons.
    std::vector<std::string> pats;
    std::string cur;
    int bracket = 0;
    for (char c : globignore) {
      if (c == '[') bracket++;
      else if (c == ']' && bracket > 0) bracket--;
      if (c == ':' && bracket == 0) { pats.push_back(cur); cur.clear(); }
      else cur += c;
    }
    pats.push_back(cur);
    matches.erase(std::remove_if(matches.begin(), matches.end(),
                                 [&](const std::string &m) {
                                   std::string base = m.substr(m.rfind('/') + 1);
                                   for (const std::string &gp : pats)
                                     if (!gp.empty() && pat_match(gp, base)) return true;
                                   return false;
                                 }),
                  matches.end());
  }
  if (matches.empty()) {
    // `shopt -s failglob': a pattern with no matches is an error that aborts
    // the whole command (bash longjmps with DISCARD; status 1).  Only the
    // first failing pattern reports -- bash never expands the rest.
    auto fg = sh_.shopt_opts.find("failglob");
    if (fg != sh_.shopt_opts.end() && fg->second) {
      if (!sh_.arith_error) {
        std::fprintf(stderr, "%sno match: %s\n", sh_.err_prefix().c_str(), field.c_str());
        sh_.arith_error = true;
      }
      return {};
    }
    auto it = sh_.shopt_opts.find("nullglob");
    if (it != sh_.shopt_opts.end() && it->second) return {};  // nullglob: remove word
    return {field};  // default: keep the pattern literally
  }
  globsort_results(sh_, matches);
  return matches;
}

namespace {
// Fork CMD connected to a pipe and return the /dev/fd path the consumer opens.
// input==true is <(cmd): the child's stdout feeds the pipe (parent reads);
// input==false is >(cmd): the child's stdin comes from the pipe (parent writes).
std::string spawn_procsub(Shell &sh, const std::string &cmd, bool input) {
  int fds[2];
  if (pipe(fds) != 0) return std::string();
  pid_t pid = fork();
  if (pid == 0) {
    if (input) { close(fds[0]); dup2(fds[1], STDOUT_FILENO); close(fds[1]); }
    else       { close(fds[1]); dup2(fds[0], STDIN_FILENO);  close(fds[0]); }
    sh.job_control = false;
    sh.subshell_level++;
    int st = sh.run_string(cmd);
    std::fflush(nullptr);
    _exit(st & 0xff);
  }
  int keep = input ? fds[0] : fds[1];
  close(input ? fds[1] : fds[0]);
  if (pid < 0) { close(keep); return std::string(); }
  sh.procsubs.push_back({static_cast<long>(pid), keep});
  // bash sets $! to the process-substitution child, so `cat <(exit 123)'
  // can be followed by `wait "$!"' (procsub1.sub).
  sh.last_bg_pid = static_cast<int>(pid);
  return "/dev/fd/" + std::to_string(keep);
}
}  // namespace

void Expander::extract_procsubs(std::string &word) {
  for (size_t i = 0; i + 1 < word.size();) {
    char c = word[i];
    if (c == '\\') { i += 2; continue; }
    if (c == '\'') { i++; while (i < word.size() && word[i] != '\'') i++; if (i < word.size()) i++; continue; }
    if (c == '"') { i++; while (i < word.size() && word[i] != '"') { if (word[i] == '\\') i++; i++; } if (i < word.size()) i++; continue; }
    // Skip $(...) / $((...)) / ${...} and `...`: a `<('/`>(' inside them is not
    // this word's process substitution (an arithmetic `4>(2+3)' is a comparison,
    // and a nested command runs its own procsubs when it executes).
    if (c == '$' && (word[i + 1] == '(' || word[i + 1] == '{')) {
      char oc = word[i + 1], cc = oc == '(' ? ')' : '}';
      int depth = 0;
      size_t j = i + 1;
      for (; j < word.size(); j++) {
        if (word[j] == oc) depth++;
        else if (word[j] == cc) { if (--depth == 0) { j++; break; } }
      }
      i = j;
      continue;
    }
    if (c == '`') {
      size_t j = i + 1;
      while (j < word.size() && word[j] != '`') { if (word[j] == '\\') j++; j++; }
      i = (j < word.size()) ? j + 1 : j;
      continue;
    }
    if ((c == '<' || c == '>') && word[i + 1] == '(') {
      int depth = 0;
      size_t j = i + 1;
      for (; j < word.size(); j++) {
        if (word[j] == '(') depth++;
        else if (word[j] == ')') { if (--depth == 0) break; }
      }
      if (j >= word.size()) { i++; continue; }  // unbalanced: leave alone
      std::string cmd = word.substr(i + 2, j - (i + 2));
      std::string path = spawn_procsub(sh_, cmd, c == '<');
      if (path.empty()) { i = j + 1; continue; }
      word = word.substr(0, i) + path + word.substr(j + 1);
      i += path.size();
    } else {
      i++;
    }
  }
}

static std::string tilde_assign(Shell &sh, const std::string &text);

std::vector<std::string> Expander::expand_args(const std::vector<Word> &words) {
  std::vector<std::string> result;
  for (const Word &w : words) {
    for (const std::string &braced : brace_expand(w.text)) {
      // A word shaped like an assignment (name=value) gets assignment-style
      // tilde expansion -- after the `=' and after each `:' -- unless posix
      // mode is on.  (bash's W_ASSIGNMENT tilde rule.)
      std::string pre = braced;
      if (!sh_.opt_posix) {
        size_t q = 0;
        while (q < pre.size() &&
               (std::isalnum(static_cast<unsigned char>(pre[q])) || pre[q] == '_'))
          q++;
        if (q > 0 && q < pre.size() && pre[q] == '=' &&
            std::isalpha(static_cast<unsigned char>(pre[0])))
          pre = pre.substr(0, q + 1) + tilde_assign(sh_, pre.substr(q + 1));
      }
      std::string tilded = expand_leading_tilde(sh_, pre);
      extract_procsubs(tilded);  // <(cmd) / >(cmd) -> /dev/fd/N
      std::string out, mask;
      bool saved_split = splitting_;
      splitting_ = true;  // this word's result is field-split
      process(tilded, out, mask, false);
      splitting_ = saved_split;
      auto fields = split_ifs(out, mask);
      // Strip quoted-null markers; a field that held only a marker survives as
      // an empty field (so "" / "$empty" yield one empty argument).
      for (auto &fm : fields) {
        std::string v, m;
        v.reserve(fm.first.size());
        m.reserve(fm.first.size());
        for (size_t k = 0; k < fm.first.size(); k++)
          if (!(fm.first[k] == QNULL && fm.second[k] == MMARK)) {
            v += fm.first[k];
            m += fm.second[k];
          }
        fm.first = std::move(v);
        fm.second = std::move(m);
      }
      for (const auto &fm : fields)
        for (const std::string &g : glob_field(fm.first, fm.second)) result.push_back(g);
    }
  }
  return result;
}

std::string Expander::expand_pattern(const std::string &text) {
  std::string src = expand_leading_tilde(sh_, text);  // `case ~ in ~)' matches
  extract_procsubs(src);
  std::string out, mask;
  bool saved_split = splitting_;
  splitting_ = false;
  process(src, out, mask, false);
  splitting_ = saved_split;
  std::string r;
  r.reserve(out.size());
  for (size_t i = 0; i < out.size(); i++) {
    char c = out[i];
    if (i < mask.size() && mask[i] == MMARK) {
      if (c == FIELD_SEP) r += ' ';
      continue;  // marker bytes never reach the pattern
    }
    if (i < mask.size() && mask[i] == '1') r += '\\';  // quoted: match literally
    r += c;
  }
  return r;
}

std::string Expander::expand_no_split(const std::string &text, bool do_glob, bool do_procsub) {
  std::string src = expand_leading_tilde(sh_, text);  // case subjects, redirects
  // Arithmetic contexts pass do_procsub=false: there `4>(2+3)' is a comparison,
  // not a `>(cmd)' process substitution.
  if (do_procsub) extract_procsubs(src);  // e.g. a redirect target: < <(cmd)
  std::string out, mask;
  bool saved_split = splitting_;
  splitting_ = false;  // result is flattened, so $* joins with IFS[0]
  process(src, out, mask, false);
  splitting_ = saved_split;
  // drop internal markers: field separators become spaces, quoted-nulls vanish
  std::string joined;
  joined.reserve(out.size());
  for (size_t k = 0; k < out.size(); k++) {
    if (k < mask.size() && mask[k] == MMARK) {
      if (out[k] == FIELD_SEP) joined += ' ';
      continue;
    }
    joined += out[k];
  }
  if (do_glob) {
    std::string fmask(joined.size(), '0');
    auto g = glob_field(joined, fmask);
    if (g.size() == 1) return g[0];
  }
  return joined;
}

std::string Expander::expand_assignment(const std::string &text) {
  return expand_no_split(tilde_assign(sh_, text));
}

void Expander::expand_word_fields(const std::string &w) {
  std::string src = expand_leading_tilde(sh_, w);
  extract_procsubs(src);
  std::string o, m;
  bool saved = splitting_;
  splitting_ = true;  // nested $*/$@ -> separate fields, with proper masks
  process(src, o, m, false);
  splitting_ = saved;
  // The whole replacement is EXPANSION OUTPUT: its unquoted literal text
  // IFS-splits like a parameter's value (`${IFS+foo 'b c' baz}' is three
  // fields), so remap literal '2' to splittable '0'; quoted '1' stays
  // protected.
  for (char &mc : m)
    if (mc == '2') mc = '0';
  op_out_ = std::move(o);
  op_mask_ = std::move(m);
  op_fields_ = true;  // consumed (and cleared) by the ${...} splice in expand_dollar
}

// True if W contains an UNQUOTED $*/$@/[@]/[*] -- a splat that expands to
// multiple fields.  A quoted "$*"/"${a[*]}" joins with IFS[0] instead, so it
// must NOT take the field-preserving path (its content would wrongly split).
static bool has_unquoted_splat(const std::string &w) {
  bool sq = false, dq = false;
  for (size_t k = 0; k < w.size(); k++) {
    char c = w[k];
    if (sq) { if (c == '\'') sq = false; continue; }
    if (c == '\\' && dq && k + 1 < w.size()) { k++; continue; }
    if (c == '"') { dq = !dq; continue; }
    if (c == '\'') { sq = true; continue; }
    if (dq) continue;
    if (c == '$' && k + 1 < w.size() && (w[k + 1] == '*' || w[k + 1] == '@')) return true;
    if (c == '[' && k + 2 < w.size() && (w[k + 1] == '*' || w[k + 1] == '@') && w[k + 2] == ']')
      return true;
  }
  return false;
}

std::string Expander::expand_op_word(const std::string &w, bool dq, bool top_level) {
  // An UNQUOTED substitute word is expanded and field-split like any other
  // expansion output, with the word's own quotes protecting their content:
  // `${IFS+foo 'b c' baz}' is three fields, the middle one `b c' (bash).
  if (top_level && splitting_ && !dq && !sh_.is_zsh() &&
      (has_unquoted_splat(w) ||
       w.find_first_of(" \t\n") != std::string::npos)) {
    expand_word_fields(w);
    return std::string();  // real content is in op_out_/op_mask_ (op_fields_ set)
  }
  // Inside double quotes only `$@' (and `${a[@]}') keeps its field structure;
  // `$*' and `${a[*]}' JOIN with IFS[0] like any other double-quoted splat, so
  // `"${var-$*}"' is one field (exp9.sub).
  bool has_at = w.find("$@") != std::string::npos || w.find("[@]") != std::string::npos;
  if (dq && !sh_.is_zsh() && !has_at) return expand_dq_word(w);
  return expand_no_split(w);
}

std::string Expander::expand_heredoc(const std::string &text) {
  std::string out, mask;
  bool saved_split = splitting_;
  splitting_ = false;
  process(text, out, mask, false, /*heredoc=*/true);  // quotes stay literal
  splitting_ = saved_split;
  std::string joined;
  for (size_t k = 0; k < out.size(); k++) {
    if (k < mask.size() && mask[k] == MMARK) {
      if (out[k] == FIELD_SEP) joined += ' ';
      continue;
    }
    joined += out[k];
  }
  return joined;
}

// ---- brace expansion ------------------------------------------------------

// Upper bound on the number of fields a single brace expansion may produce.
// Far above any real use ({1..1000000} still expands exactly as in bash), but
// low enough that a pathological range or combinatorial product cannot exhaust
// memory and abort the shell.
constexpr std::size_t kMaxBraceItems = 1000000;

std::vector<std::string> brace_expand(const std::string &text) {
  // Find the first top-level {...} containing a comma or a ..range.
  size_t open = std::string::npos;
  int depth = 0;
  for (size_t i = 0; i < text.size(); i++) {
    char c = text[i];
    if (c == '\\') { i++; continue; }
    if (c == '\'' ) { while (++i < text.size() && text[i] != '\'') {} continue; }
    if (c == '"') { while (++i < text.size() && text[i] != '"') { if (text[i]=='\\') i++; } continue; }
    // A `...` command substitution is opaque to outer brace expansion: its
    // `{'/`,' belong to the nested command, not this word.
    if (c == '`') { while (++i < text.size() && text[i] != '`') { if (text[i]=='\\') i++; } continue; }
    // Skip $-constructs so their `{'/`,' aren't treated as brace expansion.
    if (c == '$' && i + 1 < text.size() && (text[i + 1] == '{' || text[i + 1] == '(')) {
      char oc = text[i + 1], cc = oc == '{' ? '}' : ')';
      size_t m = scan_balanced(text, i + 1, oc, cc);
      i = (m == std::string::npos) ? text.size() : m;
      continue;
    }
    if (c == '{') { if (depth == 0) open = i; depth++; }
    else if (c == '}') {
      if (depth > 0) {
        depth--;
        if (depth == 0 && open != std::string::npos) {
          std::string inside = text.substr(open + 1, i - open - 1);
          // split on top-level commas
          std::vector<std::string> parts;
          int d = 0;
          std::string cur;
          bool comma = false;
          for (size_t k = 0; k < inside.size(); k++) {
            char ic = inside[k];
            // A backslash escapes the next character (`{abc\,def}' is a single
            // item, not two): keep both so later quote removal strips the `\'.
            if (ic == '\\' && k + 1 < inside.size()) { cur += ic; cur += inside[++k]; continue; }
            // A quoted comma is not a separator (`{"x,x"}' is one item): copy the
            // quoted span verbatim, leaving the quotes for later removal.
            if (ic == '\'') {
              cur += ic;
              while (++k < inside.size() && inside[k] != '\'') cur += inside[k];
              if (k < inside.size()) cur += inside[k];
              continue;
            }
            if (ic == '"') {
              cur += ic;
              while (++k < inside.size() && inside[k] != '"') {
                if (inside[k] == '\\' && k + 1 < inside.size()) cur += inside[k++];
                cur += inside[k];
              }
              if (k < inside.size()) cur += inside[k];
              continue;
            }
            if (ic == '{') d++;
            else if (ic == '}') d--;
            if (ic == ',' && d == 0) { parts.push_back(cur); cur.clear(); comma = true; }
            else cur += ic;
          }
          parts.push_back(cur);
          std::vector<std::string> items;
          if (comma) {
            items = parts;
          } else {
            // Sequence range {start..end} or {start..end..step}.  The step is
            // taken as a magnitude (its sign is ignored; direction runs from
            // start to end); a 0 or absent step means 1.
            size_t d1 = inside.find("..");
            if (d1 != std::string::npos) {
              std::string a = inside.substr(0, d1);
              std::string rest = inside.substr(d1 + 2);
              size_t d2 = rest.find("..");
              std::string b = (d2 == std::string::npos) ? rest : rest.substr(0, d2);
              std::string stepstr = (d2 == std::string::npos) ? std::string() : rest.substr(d2 + 2);
              char *ea = nullptr, *eb = nullptr, *es = nullptr;
              long va = std::strtol(a.c_str(), &ea, 10), vb = std::strtol(b.c_str(), &eb, 10);
              long step = stepstr.empty() ? 1 : std::strtol(stepstr.c_str(), &es, 10);
              bool step_ok = stepstr.empty() || (es && *es == '\0');
              if (step == LONG_MIN) step_ok = false;  // -step would overflow
              if (step == 0) step = 1;
              else if (step < 0) step = -step;
              bool a_num = ea && *ea == '\0' && !a.empty();
              bool b_num = eb && *eb == '\0' && !b.empty();
              if (a_num && b_num && step_ok) {
                // Zero-pad the terms to a common width when either bound is
                // written with a leading zero (`{00..10}' -> 00 01 ... 10).
                auto digits = [](const std::string &s) {
                  size_t p = (!s.empty() && (s[0] == '-' || s[0] == '+')) ? 1 : 0;
                  return s.substr(p);
                };
                std::string da = digits(a), db = digits(b);
                bool pad = (da.size() > 1 && da[0] == '0') || (db.size() > 1 && db[0] == '0');
                size_t width = std::max(da.size(), db.size());
                auto fmt = [&](long v) {
                  if (!pad) return std::to_string(v);
                  bool neg = v < 0;
                  std::string ds = std::to_string(neg ? -v : v);
                  while (ds.size() < width) ds = "0" + ds;
                  return (neg ? "-" : "") + ds;
                };
                unsigned long long span =
                    (va <= vb) ? static_cast<unsigned long long>(vb) - static_cast<unsigned long long>(va)
                               : static_cast<unsigned long long>(va) - static_cast<unsigned long long>(vb);
                // Iterate by element count, incrementing only between
                // elements (like bash's mkseq): a bound at LONG_MAX/LONG_MIN
                // must not be stepped past, which overflows and loops forever.
                unsigned long long count = span / static_cast<unsigned long long>(step) + 1;
                if (count <= kMaxBraceItems) {
                  long v = va;
                  for (unsigned long long j = 0;;) {
                    items.push_back(fmt(v));
                    if (++j >= count) break;
                    if (va <= vb) v += step; else v -= step;
                  }
                }
              } else if (a.size() == 1 && b.size() == 1 && step_ok &&
                         std::isalpha(static_cast<unsigned char>(a[0])) &&
                         std::isalpha(static_cast<unsigned char>(b[0]))) {
                char ca = a[0], cb = b[0];
                // A character range yields LITERAL characters: bash brace-
                // expands after parsing, so a `$', backquote, or quote in the
                // range is never re-scanned as syntax.  Escape those here so
                // the expander treats them as data (`{Z..a}' -- braces.tests).
                auto lit = [](int v) {
                  std::string s;
                  char c2 = static_cast<char>(v);
                  // A backslash is quote-removed to nothing, but the field
                  // survives (bash prints an empty word); an empty pair of
                  // quotes reproduces that in both bare and joined contexts.
                  if (c2 == '\\') return std::string("\"\"");
                  if (c2 == '$' || c2 == '`' || c2 == '"' || c2 == '\'' || c2 == '!')
                    s += '\\';
                  s += c2;
                  return s;
                };
                if (ca <= cb) for (int v = ca; v <= cb; v += step) items.push_back(lit(v));
                else for (int v = ca; v >= cb; v -= step) items.push_back(lit(v));
              }
            }
          }
          if (!items.empty()) {
            std::string pre = text.substr(0, open);
            std::string post = text.substr(i + 1);
            std::vector<std::string> out;
            for (const std::string &it : items)
              for (const std::string &tail : brace_expand(it + post)) {
                // Cap the combinatorial product ({a,b}{a,b}... grows as 2^n);
                // beyond the cap, leave the word unexpanded rather than exhaust
                // memory and abort.
                if (out.size() >= kMaxBraceItems) return {text};
                out.push_back(pre + tail);
              }
            return out;
          }
          // The outer {...} is not itself a brace expression (no top-level comma
          // or valid range), so its braces are literal -- but an inner brace may
          // still expand: `a-{b{d,e}}-c' -> a-{bd}-c a-{be}-c.  Recurse on the
          // interior; if it expands, keep the literal outer braces around each
          // result and combine with the (recursively expanded) postscript.
          {
            std::vector<std::string> inner = brace_expand(inside);
            bool changed = inner.size() > 1 || (inner.size() == 1 && inner[0] != inside);
            if (changed) {
              std::string pre = text.substr(0, open);
              std::string post = text.substr(i + 1);
              std::vector<std::string> out;
              for (const std::string &ie : inner)
                for (const std::string &tail : brace_expand(post)) {
                  if (out.size() >= kMaxBraceItems) return {text};
                  out.push_back(pre + "{" + ie + "}" + tail);
                }
              return out;
            }
          }
          open = std::string::npos;
        }
      }
    }
  }
  // An unmatched `{' (never balanced by a `}') is literal, but a balanced brace
  // nested after it may still expand: `a-{bdef-{g,i}-c' -> a-{bdef-g-c
  // a-{bdef-i-c'.  Re-expand the text following the stray `{' and keep it as a
  // literal prefix.
  if (open != std::string::npos && open + 1 < text.size()) {
    std::string rest = text.substr(open + 1);
    std::vector<std::string> re = brace_expand(rest);
    if (re.size() > 1 || (re.size() == 1 && re[0] != rest)) {
      std::string pre = text.substr(0, open + 1);
      std::vector<std::string> out;
      for (const std::string &r : re) {
        if (out.size() >= kMaxBraceItems) return {text};
        out.push_back(pre + r);
      }
      return out;
    }
  }
  return {text};
}

}  // namespace gnash::core
