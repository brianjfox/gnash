// prompt.cpp -- PS1/PS2 prompt-string expansion (bash backslash escapes).

#include "gnash/core/shell.hpp"

#include <cstdlib>
#include <ctime>
#include <pwd.h>
#include <unistd.h>

namespace gnash::core {

namespace {
std::string home_relative(const std::string &cwd, const std::string &home) {
  if (!home.empty() && cwd.rfind(home, 0) == 0)
    return "~" + cwd.substr(home.size());
  return cwd;
}
}  // namespace

std::string expand_prompt(Shell &sh, const std::string &ps) {
  std::string out;
  for (size_t i = 0; i < ps.size(); i++) {
    if (ps[i] != '\\') {
      out += ps[i];
      continue;
    }
    if (i + 1 >= ps.size()) { out += '\\'; break; }
    char c = ps[++i];
    switch (c) {
      case 'u': {
        struct passwd *pw = getpwuid(getuid());
        out += pw ? pw->pw_name : (std::getenv("USER") ? std::getenv("USER") : "");
        break;
      }
      case 'h': case 'H': {
        char host[256] = {0};
        gethostname(host, sizeof host - 1);
        std::string h = host;
        if (c == 'h') { size_t d = h.find('.'); if (d != std::string::npos) h = h.substr(0, d); }
        out += h;
        break;
      }
      case 'w': case 'W': {
        std::string dir = sh.get("PWD");  // logical path, as bash uses
        if (dir.empty() || dir[0] != '/') {
          char cwd[4096];
          dir = getcwd(cwd, sizeof cwd) ? cwd : "";
        }
        std::string home = sh.get("HOME");
        dir = home_relative(dir, home);
        if (c == 'W') { size_t s = dir.find_last_of('/'); if (s != std::string::npos && dir != "/") dir = dir.substr(s + 1); }
        out += dir;
        break;
      }
      case '$': out += (getuid() == 0 ? '#' : '$'); break;
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 'a': out += '\a'; break;
      case 'e': out += '\033'; break;
      case 's': {
        std::string a0 = sh.arg0;
        size_t s = a0.find_last_of('/');
        out += (s == std::string::npos) ? a0 : a0.substr(s + 1);
        break;
      }
      case 'v': case 'V': out += "0.1"; break;
      case 't': case 'T': case '@': case 'A': {
        std::time_t now = std::time(nullptr);
        std::tm tmv;
        localtime_r(&now, &tmv);
        char buf[32];
        const char *fmt = (c == 'A') ? "%H:%M" : (c == '@') ? "%I:%M %p" : (c == 'T') ? "%I:%M:%S" : "%H:%M:%S";
        std::strftime(buf, sizeof buf, fmt, &tmv);
        out += buf;
        break;
      }
      case 'd': {
        std::time_t now = std::time(nullptr);
        std::tm tmv;
        localtime_r(&now, &tmv);
        char buf[32];
        std::strftime(buf, sizeof buf, "%a %b %d", &tmv);
        out += buf;
        break;
      }
      case '[': case ']': break;  // non-printing markers
      case '\\': out += '\\'; break;
      default: out += '\\'; out += c; break;
    }
  }
  return out;
}

}  // namespace gnash::core
