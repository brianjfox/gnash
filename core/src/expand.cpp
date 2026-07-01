// expand.cpp -- word expansion (see expand.hpp).
//
// A pragmatic but faithful implementation of the bash expansion pipeline.  Not
// yet covered: arrays (${a[@]}), ${!prefix*}, some locale/case operators; these
// are follow-ons.

#include "gnash/core/expand.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <pwd.h>
#include <unistd.h>

#include "gnash/glob.hpp"
#include "strmatch.h"

namespace gnash::core {

namespace {

constexpr char FIELD_SEP = '\x01';  // internal "$@" field boundary marker

bool pat_match(const std::string &pattern, const std::string &text) {
  std::string p = pattern, t = text;
  return strmatch(p.data(), t.data(), FNM_EXTMATCH) == 0;
}

// Scan a balanced span from `open`/`close` starting with text[i] at the opener;
// returns the index of the matching closer (or npos).  Honors quotes.
size_t scan_balanced(const std::string &t, size_t i, char open, char close) {
  int depth = 0;
  for (; i < t.size(); i++) {
    char c = t[i];
    if (c == '\\') { i++; continue; }
    if (c == '\'') { while (++i < t.size() && t[i] != '\'') {} continue; }
    if (c == '"') {
      while (++i < t.size() && t[i] != '"')
        if (t[i] == '\\') i++;
      continue;
    }
    if (c == open) depth++;
    else if (c == close) { if (--depth == 0) return i; }
  }
  return std::string::npos;
}

std::string ansi_c(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\\' && i + 1 < s.size()) {
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
        case 'e': out += '\033'; break;
        default: out += '\\'; out += c; break;
      }
    } else {
      out += s[i];
    }
  }
  return out;
}

}  // namespace

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
  } else {
    struct passwd *pw = getpwnam(prefix.c_str());
    if (pw) home = pw->pw_dir;
  }
  if (home.empty()) return w;
  return home + (slash == std::string::npos ? std::string() : w.substr(slash));
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

std::string Expander::param_value(const std::string &name, bool &set) {
  set = true;
  if (name == "?") return std::to_string(sh_.last_status);
  if (name == "$") return sh_.get("$");
  if (name == "!") return std::to_string(sh_.last_bg_pid);
  if (name == "#") return std::to_string(sh_.positional.size());
  if (name == "0") return sh_.arg0;
  if (name == "-") {
    std::string f;
    if (sh_.opt_errexit) f += 'e';
    if (sh_.opt_noglob) f += 'f';
    if (sh_.opt_nounset) f += 'u';
    if (sh_.opt_xtrace) f += 'x';
    if (sh_.opt_verbose) f += 'v';
    return f;
  }
  if (!name.empty() && std::isdigit(static_cast<unsigned char>(name[0]))) {
    size_t idx = static_cast<size_t>(std::atoi(name.c_str()));
    if (idx >= 1 && idx <= sh_.positional.size()) return sh_.positional[idx - 1];
    set = false;
    return std::string();
  }
  std::string v;
  if (sh_.get_if_set(name, v)) return v;
  if (sh_.dynamic_var(name, v)) return v;  // RANDOM/SECONDS/LINENO/BASHPID/EPOCH*
  set = false;
  if (sh_.opt_nounset) {
    std::fprintf(stderr, "gnash: %s: unbound variable\n", name.c_str());
    sh_.exiting = true;
    sh_.exit_status = 1;
  }
  return std::string();
}

// Expand a ${...} body (without the braces).  Returns the value; sets `split`
// if the result is subject to word splitting (always, for consistency here).
static std::string expand_brace_body(Expander &, Shell &, const std::string &);

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

void Expander::expand_dollar(const std::string &t, size_t &i, bool dq, std::string &out,
                             std::string &mask) {
  char qm = dq ? '1' : '0';
  // i is at '$'
  char n1 = i + 1 < t.size() ? t[i + 1] : '\0';

  // $((expr))
  if (n1 == '(' && i + 2 < t.size() && t[i + 2] == '(') {
    size_t p = i + 3;
    int depth = 2;
    size_t end = std::string::npos;
    for (; p < t.size(); p++) {
      if (t[p] == '(') depth++;
      else if (t[p] == ')') { if (--depth == 0) { end = p; break; } }
    }
    if (end != std::string::npos) {
      std::string expr = t.substr(i + 3, (end - 1) - (i + 3));
      bool ok = true;
      long long v = eval_arith(sh_, expand_no_split(expr), &ok);
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
      long long v = eval_arith(sh_, expand_no_split(expr), &ok);
      std::string s = std::to_string(v);
      for (char c : s) { out += c; mask += qm; }
      i = end + 1;
      return;
    }
  }
  // $(cmd)
  if (n1 == '(') {
    size_t end = scan_balanced(t, i + 1, '(', ')');
    if (end != std::string::npos) {
      std::string inner = t.substr(i + 2, end - (i + 2));
      int st = 0;
      std::string res = sh_.run_and_capture(inner, &st);
      sh_.last_status = st;
      for (char c : res) { out += c; mask += qm; }
      i = end + 1;
      return;
    }
  }
  // ${...}
  if (n1 == '{') {
    size_t end = scan_balanced(t, i + 1, '{', '}');
    if (end != std::string::npos) {
      std::string body = t.substr(i + 2, end - (i + 2));
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
            for (size_t k = 0; k < items.size(); k++) {
              if (k) { out += FIELD_SEP; mask += '0'; }
              for (char c : items[k]) { out += c; mask += '1'; }
            }
          } else if (sel == '*' && dq) {
            std::string is = sh_.ifs();
            std::string j = is.empty() ? std::string() : std::string(1, is[0]);
            for (size_t k = 0; k < items.size(); k++) {
              if (k) for (char c : j) { out += c; mask += '1'; }
              for (char c : items[k]) { out += c; mask += '1'; }
            }
          } else {
            for (size_t k = 0; k < items.size(); k++) {
              if (k) { out += FIELD_SEP; mask += '0'; }
              for (char c : items[k]) { out += c; mask += '0'; }
            }
          }
        }
        i = end + 1;
        return;
      }
      std::string val = expand_brace_body(*this, sh_, body);
      for (char c : val) { out += c; mask += qm; }
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
      for (size_t k = 0; k < pos.size(); k++) {
        if (k) { out += FIELD_SEP; mask += '0'; }
        for (char c : pos[k]) { out += c; mask += '1'; }
      }
    } else if (n1 == '*' && dq) {
      std::string sep = sh_.ifs();
      std::string joiner = sep.empty() ? std::string() : std::string(1, sep[0]);
      for (size_t k = 0; k < pos.size(); k++) {
        if (k) for (char c : joiner) { out += c; mask += '1'; }
        for (char c : pos[k]) { out += c; mask += '1'; }
      }
    } else {  // unquoted $@ or $*
      for (size_t k = 0; k < pos.size(); k++) {
        if (k) { out += FIELD_SEP; mask += '0'; }
        for (char c : pos[k]) { out += c; mask += '0'; }
      }
    }
    i += 2;
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
  bool set = false;
  std::string v = param_value(name, set);
  for (char c : v) { out += c; mask += qm; }
}

// Parse and apply ${...} operators.
static std::string expand_brace_body(Expander &ex, Shell &sh, const std::string &body) {
  // Leading `#' means length-of.
  bool length = false;
  std::string b = body;
  if (b.size() > 1 && b[0] == '#') { length = true; b = b.substr(1); }

  size_t p = 0;
  std::string name;
  if (p < b.size() && (b[p] == '@' || b[p] == '*' || b[p] == '?' || b[p] == '$' ||
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
    size_t s = p + 1;
    int d = 1;
    p++;
    while (p < b.size() && d) {
      if (b[p] == '[') d++;
      else if (b[p] == ']') d--;
      if (d) p++;
    }
    sub = b.substr(s, p - s);
    have_sub = true;
    if (p < b.size() && b[p] == ']') p++;
  }

  bool set = false;
  std::string val;
  if (have_sub) {
    val = sh.array_get(name, ex.expand_no_split(sub));
    set = sh.is_set(name);
  } else {
    val = ex.param_value(name, set);
  }
  if (length) return std::to_string(val.size());
  std::string rest = b.substr(p);

  if (rest.empty()) return val;

  auto expand_word = [&](const std::string &w) { return ex.expand_no_split(w); };

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
    if (op == '-') return empty ? expand_word(word) : val;
    if (op == '+') return empty ? std::string() : expand_word(word);
    if (op == '=') {
      if (empty) {
        std::string w = expand_word(word);
        sh.set(name, w);
        return w;
      }
      return val;
    }
    if (op == '?') {
      if (empty) return std::string();  // (error message elided)
      return val;
    }
  }

  // ${name#pat} ${name##pat} ${name%pat} ${name%%pat}
  if (rest[0] == '#' || rest[0] == '%') {
    bool longest = rest.size() > 1 && rest[1] == rest[0];
    std::string pat = ex.expand_no_split(rest.substr(longest ? 2 : 1));
    if (rest[0] == '#') {  // prefix removal
      if (longest) {
        for (size_t k = val.size(); k + 1 > 0; k--) {
          if (pat_match(pat, val.substr(0, k))) return val.substr(k);
          if (k == 0) break;
        }
      } else {
        for (size_t k = 0; k <= val.size(); k++)
          if (pat_match(pat, val.substr(0, k))) return val.substr(k);
      }
    } else {  // suffix removal
      if (longest) {
        for (size_t k = 0; k <= val.size(); k++)
          if (pat_match(pat, val.substr(k))) return val.substr(0, k);
      } else {
        for (size_t k = val.size(); k + 1 > 0; k--) {
          if (pat_match(pat, val.substr(k))) return val.substr(0, k);
          if (k == 0) break;
        }
      }
    }
    return val;
  }

  // ${name/pat/rep} ${name//pat/rep}
  if (rest[0] == '/') {
    bool global = rest.size() > 1 && rest[1] == '/';
    std::string body2 = rest.substr(global ? 2 : 1);
    size_t slash = std::string::npos;
    for (size_t k = 0; k < body2.size(); k++) {
      if (body2[k] == '\\') { k++; continue; }
      if (body2[k] == '/') { slash = k; break; }
    }
    std::string pat = ex.expand_no_split(slash == std::string::npos ? body2 : body2.substr(0, slash));
    std::string rep = slash == std::string::npos ? std::string()
                                                 : ex.expand_no_split(body2.substr(slash + 1));
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
    std::string pat = ex.expand_no_split(rest.substr(all ? 2 : 1));
    bool up = rest[0] == '^';
    std::string out;
    bool first = true;
    for (char c : val) {
      std::string cs(1, c);
      bool m = pat.empty() ? true : pat_match(pat, cs);
      char nc = c;
      if (m && (all || first))
        nc = up ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (!all && first) first = false;
      out += nc;
    }
    return out;
  }

  // ${name:offset:length}
  if (rest[0] == ':') {
    std::string args = rest.substr(1);
    size_t colon2 = args.find(':');
    bool ok = true;
    long long off = eval_arith(sh, colon2 == std::string::npos ? args : args.substr(0, colon2), &ok);
    long long len = -1;
    if (colon2 != std::string::npos) len = eval_arith(sh, args.substr(colon2 + 1), &ok);
    long long n = static_cast<long long>(val.size());
    if (off < 0) off += n;
    if (off < 0) off = 0;
    if (off > n) off = n;
    std::string res = val.substr(static_cast<size_t>(off));
    if (len >= 0 && len < static_cast<long long>(res.size()))
      res = res.substr(0, static_cast<size_t>(len));
    return res;
  }

  return val;
}

void Expander::process(const std::string &text, std::string &out, std::string &mask,
                       bool /*assignment_rhs*/) {
  size_t i = 0;
  while (i < text.size()) {
    char c = text[i];
    if (c == '\'') {
      i++;
      while (i < text.size() && text[i] != '\'') { out += text[i]; mask += '1'; i++; }
      if (i < text.size()) i++;
    } else if (c == '"') {
      i++;
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
          for (char ch : res) { out += ch; mask += '1'; }
          i = (j < text.size()) ? j + 1 : j;
        } else {
          out += text[i];
          mask += '1';
          i++;
        }
      }
      if (i < text.size()) i++;
    } else if (c == '$' && i + 1 < text.size() && text[i + 1] == '\'') {
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
      if (i + 1 < text.size()) { out += text[i + 1]; mask += '1'; i += 2; }
      else i++;
    } else if (c == '$') {
      expand_dollar(text, i, false, out, mask);
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
      for (char ch : res) { out += ch; mask += '0'; }
      i = (j < text.size()) ? j + 1 : j;
    } else {
      out += c;
      mask += '0';
      i++;
    }
  }
}

std::vector<std::string> Expander::split_ifs(const std::string &s, const std::string &mask) {
  std::string ifs = sh_.ifs();
  std::vector<std::string> fields;
  std::string cur;
  bool have = false;
  auto is_ifs = [&](char c) { return ifs.find(c) != std::string::npos; };
  auto is_ws = [&](char c) { return c == ' ' || c == '\t' || c == '\n'; };
  size_t i = 0;
  while (i < s.size()) {
    char c = s[i];
    bool q = mask[i] == '1';
    if (!q && c == FIELD_SEP) {
      fields.push_back(cur);
      cur.clear();
      have = false;
      i++;
      continue;
    }
    if (!q && is_ifs(c)) {
      if (is_ws(c)) {
        if (have) { fields.push_back(cur); cur.clear(); have = false; }
        while (i < s.size() && mask[i] != '1' && is_ifs(s[i]) && is_ws(s[i])) i++;
      } else {
        fields.push_back(cur);
        cur.clear();
        have = false;
        i++;
      }
      continue;
    }
    cur += c;
    have = true;
    i++;
  }
  if (have) fields.push_back(cur);
  return fields;
}

std::vector<std::string> Expander::glob_field(const std::string &field, const std::string &mask) {
  // Build a glob pattern: unquoted metacharacters stay special, quoted ones are
  // backslash-escaped; also produce the literal (quotes already removed).
  std::string pattern;
  bool magic = false;
  for (size_t i = 0; i < field.size(); i++) {
    char c = field[i];
    bool q = mask[i] == '1';
    if (!q && (c == '*' || c == '?' || c == '[')) magic = true;
    if (q && (c == '*' || c == '?' || c == '[' || c == '\\' || c == ']')) pattern += '\\';
    pattern += c;
  }
  if (sh_.opt_noglob || !magic) return {field};
  auto matches = gnash::glob::glob(pattern, 0);
  if (matches.empty()) return {field};  // nullglob off: keep literal
  return matches;
}

std::vector<std::string> Expander::expand_args(const std::vector<Word> &words) {
  std::vector<std::string> result;
  for (const Word &w : words) {
    for (const std::string &braced : brace_expand(w.text)) {
      std::string tilded = expand_leading_tilde(sh_, braced);
      std::string out, mask;
      process(tilded, out, mask, false);
      for (const std::string &field : split_ifs(out, mask)) {
        // recompute a mask for the field for globbing: chars from split lost
        // their per-char mask, so treat all as unquoted (glob may act). This is
        // a simplification; quoted metachars in split output are rare.
        std::string fmask(field.size(), '0');
        for (const std::string &g : glob_field(field, fmask)) result.push_back(g);
      }
    }
  }
  return result;
}

std::string Expander::expand_no_split(const std::string &text, bool do_glob) {
  std::string out, mask;
  process(text, out, mask, false);
  // drop field-separator markers
  std::string joined;
  for (char c : out)
    if (c != FIELD_SEP) joined += c; else joined += ' ';
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

// ---- brace expansion ------------------------------------------------------

std::vector<std::string> brace_expand(const std::string &text) {
  // Find the first top-level {...} containing a comma or a ..range.
  size_t open = std::string::npos;
  int depth = 0;
  for (size_t i = 0; i < text.size(); i++) {
    char c = text[i];
    if (c == '\\') { i++; continue; }
    if (c == '\'' ) { while (++i < text.size() && text[i] != '\'') {} continue; }
    if (c == '"') { while (++i < text.size() && text[i] != '"') { if (text[i]=='\\') i++; } continue; }
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
            // numeric or char range {a..b}
            size_t dots = inside.find("..");
            if (dots != std::string::npos) {
              std::string a = inside.substr(0, dots), b = inside.substr(dots + 2);
              char *ea = nullptr, *eb = nullptr;
              long va = std::strtol(a.c_str(), &ea, 10), vb = std::strtol(b.c_str(), &eb, 10);
              if (ea && *ea == '\0' && eb && *eb == '\0' && !a.empty() && !b.empty()) {
                if (va <= vb) for (long v = va; v <= vb; v++) items.push_back(std::to_string(v));
                else for (long v = va; v >= vb; v--) items.push_back(std::to_string(v));
              } else if (a.size() == 1 && b.size() == 1 &&
                         std::isalpha(static_cast<unsigned char>(a[0])) &&
                         std::isalpha(static_cast<unsigned char>(b[0]))) {
                char ca = a[0], cb = b[0];
                if (ca <= cb) for (char v = ca; v <= cb; v++) items.push_back(std::string(1, v));
                else for (char v = ca; v >= cb; v--) items.push_back(std::string(1, v));
              }
            }
          }
          if (!items.empty()) {
            std::string pre = text.substr(0, open);
            std::string post = text.substr(i + 1);
            std::vector<std::string> out;
            for (const std::string &it : items)
              for (const std::string &tail : brace_expand(it + post))
                out.push_back(pre + tail);
            return out;
          }
          open = std::string::npos;
        }
      }
    }
  }
  return {text};
}

}  // namespace gnash::core
