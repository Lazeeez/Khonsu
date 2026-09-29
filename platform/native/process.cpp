#include "process.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <spawn.h>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace khonsu::native {

namespace {

std::atomic<pid_t> g_child{0};

void forward_signal(int sig) {
  const pid_t pid = g_child.load();
  if (pid > 0) ::kill(pid, sig);  // async-signal-safe
}

}  // namespace

std::string shell_join(const std::vector<std::string>& argv) {
  std::string out;
  for (const auto& a : argv) {
    if (!out.empty()) out += ' ';
    const bool plain = !a.empty() && a.find_first_not_of(
                                         "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_./=:@%+,") ==
                                         std::string::npos;
    if (plain) {
      out += a;
      continue;
    }
    out += '\'';
    for (const char c : a) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    out += '\'';
  }
  return out;
}

std::string current_dir() {
  char buf[4096];
  return ::getcwd(buf, sizeof buf) ? std::string(buf) : std::string();
}

int run_child(const std::vector<std::string>& argv, const std::vector<std::pair<std::string, std::string>>& env_overrides,
              std::string* error) {
  if (argv.empty()) {
    if (error) *error = "no command given";
    return -1;
  }
  // Environment: inherit, then override.
  std::vector<std::string> env;
  for (char** e = environ; *e; ++e) {
    const std::string_view kv(*e);
    const auto eq = kv.find('=');
    const auto name = kv.substr(0, eq);
    const bool overridden = std::any_of(env_overrides.begin(), env_overrides.end(),
                                        [&](const auto& o) { return o.first == name; });
    if (!overridden) env.emplace_back(kv);
  }
  for (const auto& [k, v] : env_overrides) env.push_back(k + "=" + v);

  std::vector<char*> cargv, cenv;
  for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
  cargv.push_back(nullptr);
  for (auto& e : env) cenv.push_back(e.data());
  cenv.push_back(nullptr);

  struct sigaction sa {};
  sa.sa_handler = forward_signal;
  sigemptyset(&sa.sa_mask);
  struct sigaction old_int {}, old_term {}, old_hup {};
  ::sigaction(SIGINT, &sa, &old_int);
  ::sigaction(SIGTERM, &sa, &old_term);
  ::sigaction(SIGHUP, &sa, &old_hup);

  pid_t pid = 0;
  const int rc = ::posix_spawnp(&pid, cargv[0], nullptr, nullptr, cargv.data(), cenv.data());
  if (rc != 0) {
    if (error) *error = argv[0] + ": " + std::strerror(rc);
    ::sigaction(SIGINT, &old_int, nullptr);
    ::sigaction(SIGTERM, &old_term, nullptr);
    ::sigaction(SIGHUP, &old_hup, nullptr);
    return -1;
  }
  g_child.store(pid);
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  g_child.store(0);
  ::sigaction(SIGINT, &old_int, nullptr);
  ::sigaction(SIGTERM, &old_term, nullptr);
  ::sigaction(SIGHUP, &old_hup, nullptr);
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return 1;
}

}  // namespace khonsu::native
