// Khonsu — run the agent as a child process with the proxy injected into its env.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace khonsu::native {

// Spawns argv with the current environment plus `env_overrides`, forwards
// SIGINT/SIGTERM/SIGHUP to it, waits, and returns a shell-style exit code
// (128 + signal if it was killed). Returns -1 with `error` set if it could not start.
int run_child(const std::vector<std::string>& argv, const std::vector<std::pair<std::string, std::string>>& env_overrides,
              std::string* error);

// Shell-quoted rendering of argv for logs and recordings.
[[nodiscard]] std::string shell_join(const std::vector<std::string>& argv);

[[nodiscard]] std::string current_dir();

}  // namespace khonsu::native
