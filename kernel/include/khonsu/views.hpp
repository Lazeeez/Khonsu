// Khonsu — JSON views of recordings, shared by the CLI (--json) and the web viewer.
#pragma once

#include "khonsu/recording.hpp"
#include "khonsu/rundiff.hpp"

#include <string>

namespace khonsu::views {

// Run metadata, every call (summarized, with the workspace changes it caused),
// and every snapshot. What `khonsu show --json` prints.
[[nodiscard]] std::string recording(const rec::Recording& r);

// One call in full: request and response bodies, stream chunk timing.
[[nodiscard]] std::string call(const rec::Recording& r, std::size_t index);

// Files in a snapshot (by root id hex); what `khonsu files` lists.
[[nodiscard]] std::string files(const rec::Recording& r, const std::string& root_hex);

// One file's content at a snapshot: {"path","size","binary","text"|null,"stored"}.
[[nodiscard]] std::string file(const rec::Recording& r, const std::string& root_hex, const std::string& path);

// Workspace changes between two snapshots, with text before/after for small text files.
[[nodiscard]] std::string changes(const rec::Recording& r, const std::string& from_hex, const std::string& to_hex);

// `khonsu diff --json`.
[[nodiscard]] std::string diff(const rec::RunDiff& d, const rec::Recording& a, const rec::Recording& b);

}  // namespace khonsu::views
