// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

#include "gnash/sh/shellenv.hpp"

#include <cstdlib>
#include <pwd.h>
#include <unistd.h>

#include "gnash/sh/xmalloc.hpp"

namespace gnash::sh {

static char *default_env_provider(const char *name) { return std::getenv(name); }

// Non-anonymous so the extern "C" shim below can reach it.
EnvProvider g_env_provider = default_env_provider;

void set_env_provider(EnvProvider p) {
  g_env_provider = p ? p : default_env_provider;
}

}  // namespace gnash::sh

extern "C" char *sh_get_env_value(const char *name) {
  return gnash::sh::g_env_provider(name);
}

extern "C" char *sh_get_home_dir(void) {
  static char *home_dir = nullptr;
  if (home_dir) return home_dir;
  struct passwd *entry = getpwuid(getuid());
  if (entry && entry->pw_dir) home_dir = gnash::sh::savestring(entry->pw_dir);
  return home_dir;
}
