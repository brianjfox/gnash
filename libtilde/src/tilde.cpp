// tilde.cpp -- tilde expansion (~/foo := $HOME/foo).
//
// Faithful port of bash 5.3 lib/tilde/tilde.c.  The public entry points keep
// the classic C signatures (declared in <readline/tilde.h>); a std::string
// wrapper is layered on top in namespace gnash::tilde.

#include <cstring>
#include <pwd.h>
#include <sys/types.h>

#include "gnash/sh/shellenv.hpp"
#include "gnash/sh/xmalloc.hpp"
#include "gnash/tilde.hpp"
#include "readline/tilde.h"

using gnash::sh::savestring;
using gnash::sh::xfree;
using gnash::sh::xmalloc;
using gnash::sh::xrealloc;

// Default additional prefixes: whitespace preceding a tilde.
static const char *default_prefixes[] = {" ~", "\t~", nullptr};
// Default additional suffixes: whitespace or newline.
static const char *default_suffixes[] = {" ", "\n", nullptr};

extern "C" {
tilde_hook_func_t *tilde_expansion_preexpansion_hook = nullptr;
tilde_hook_func_t *tilde_expansion_failure_hook = nullptr;
char **tilde_additional_prefixes = const_cast<char **>(default_prefixes);
char **tilde_additional_suffixes = const_cast<char **>(default_suffixes);
}

namespace {

// Index of the tilde that starts an expansion; *len gets the length of the
// text (excluding the tilde) that identified this starter.
int tilde_find_prefix(const char *string, int *len) {
  int string_len = static_cast<int>(std::strlen(string));
  char **prefixes = tilde_additional_prefixes;

  *len = 0;
  if (*string == '\0' || *string == '~') return 0;

  if (prefixes) {
    for (int i = 0; i < string_len; i++) {
      for (int j = 0; prefixes[j]; j++) {
        size_t plen = std::strlen(prefixes[j]);
        if (std::strncmp(string + i, prefixes[j], plen) == 0) {
          *len = static_cast<int>(plen) - 1;
          return i + *len;
        }
      }
    }
  }
  return string_len;
}

// Index of the character that ends the tilde definition.
int tilde_find_suffix(const char *string) {
  int string_len = static_cast<int>(std::strlen(string));
  char **suffixes = tilde_additional_suffixes;
  int i;

  for (i = 0; i < string_len; i++) {
    if (string[i] == '/') break;
    for (int j = 0; suffixes && suffixes[j]; j++) {
      if (std::strncmp(string + i, suffixes[j], std::strlen(suffixes[j])) == 0)
        return i;
    }
  }
  return i;
}

// Return FNAME's tilde prefix (the username), setting *lenp to its end index.
char *isolate_tilde_prefix(const char *fname, int *lenp) {
  char *ret = static_cast<char *>(xmalloc(std::strlen(fname)));
  int i;
  for (i = 1; fname[i] && fname[i] != '/'; i++) ret[i - 1] = fname[i];
  ret[i - 1] = '\0';
  if (lenp) *lenp = i;
  return ret;
}

// PREFIX concatenated with SUFFIX starting at SUFFIND.
char *glue_prefix_and_suffix(const char *prefix, const char *suffix, int suffind) {
  size_t plen = (prefix && *prefix) ? std::strlen(prefix) : 0;
  size_t slen = std::strlen(suffix + suffind);
  char *ret = static_cast<char *>(xmalloc(plen + slen + 1));
  if (plen) std::strcpy(ret, prefix);
  std::strcpy(ret + plen, suffix + suffind);
  return ret;
}

}  // namespace

extern "C" char *tilde_expand_word(const char *filename) {
  char *dirname, *expansion, *username;
  int user_len;
  struct passwd *user_entry;

  if (filename == nullptr) return nullptr;
  if (*filename != '~') return savestring(filename);

  // A leading `~/' or bare `~' always maps to $HOME / the current user's home.
  if (filename[1] == '\0' || filename[1] == '/') {
    expansion = sh_get_env_value("HOME");
    if (expansion == nullptr) expansion = sh_get_home_dir();
    return glue_prefix_and_suffix(expansion, filename, 1);
  }

  username = isolate_tilde_prefix(filename, &user_len);

  if (tilde_expansion_preexpansion_hook) {
    expansion = (*tilde_expansion_preexpansion_hook)(username);
    if (expansion) {
      dirname = glue_prefix_and_suffix(expansion, filename, user_len);
      xfree(username);
      xfree(expansion);
      return dirname;
    }
  }

  dirname = nullptr;
  user_entry = getpwnam(username);
  if (user_entry == nullptr) {
    if (tilde_expansion_failure_hook) {
      expansion = (*tilde_expansion_failure_hook)(username);
      if (expansion) {
        dirname = glue_prefix_and_suffix(expansion, filename, user_len);
        xfree(expansion);
      }
    }
    if (dirname == nullptr) dirname = savestring(filename);
  } else {
    dirname = glue_prefix_and_suffix(user_entry->pw_dir, filename, user_len);
  }

  xfree(username);
  endpwent();
  return dirname;
}

extern "C" char *tilde_expand(const char *string) {
  char *result;
  size_t result_size, result_index;

  result_index = result_size = 0;
  if (std::strchr(string, '~'))
    result = static_cast<char *>(xmalloc(result_size = std::strlen(string) + 16));
  else
    result = static_cast<char *>(xmalloc(result_size = std::strlen(string) + 1));

  while (1) {
    int start, end, len;
    char *tilde_word, *expansion;

    start = tilde_find_prefix(string, &len);

    if ((result_index + static_cast<size_t>(start) + 1) > result_size)
      result = static_cast<char *>(
          xrealloc(result, 1 + (result_size += static_cast<size_t>(start) + 20)));

    std::strncpy(result + result_index, string, static_cast<size_t>(start));
    result_index += static_cast<size_t>(start);
    string += start;

    end = tilde_find_suffix(string);
    if (!start && !end) break;

    tilde_word = static_cast<char *>(xmalloc(static_cast<size_t>(1 + end)));
    std::strncpy(tilde_word, string, static_cast<size_t>(end));
    tilde_word[end] = '\0';
    string += end;

    expansion = tilde_expand_word(tilde_word);
    if (expansion == nullptr)
      expansion = tilde_word;
    else
      xfree(tilde_word);

    len = static_cast<int>(std::strlen(expansion));
    if ((result_index + static_cast<size_t>(len) + 1) > result_size)
      result = static_cast<char *>(
          xrealloc(result, 1 + (result_size += static_cast<size_t>(len) + 20)));
    std::strcpy(result + result_index, expansion);
    result_index += static_cast<size_t>(len);
    xfree(expansion);
  }

  result[result_index] = '\0';
  return result;
}

extern "C" char *tilde_find_word(const char *fname, int /*flags*/, int *lenp) {
  int x = tilde_find_suffix(fname);
  char *r;
  if (x == 0) {
    r = savestring(fname);
    if (lenp) *lenp = 0;
  } else {
    r = static_cast<char *>(xmalloc(static_cast<size_t>(1 + x)));
    std::strncpy(r, fname, static_cast<size_t>(x));
    r[x] = '\0';
    if (lenp) *lenp = x;
  }
  return r;
}

// ---- C++ convenience wrappers ---------------------------------------------
namespace gnash::tilde {

std::string expand(std::string_view s) {
  std::string in(s);
  char *r = tilde_expand(in.c_str());
  std::string out(r);
  gnash::sh::xfree(r);
  return out;
}

std::string expand_word(std::string_view s) {
  std::string in(s);
  char *r = tilde_expand_word(in.c_str());
  std::string out(r ? r : "");
  gnash::sh::xfree(r);
  return out;
}

}  // namespace gnash::tilde
