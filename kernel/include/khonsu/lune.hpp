// Khonsu — Lune: a tiny deterministic VM whose running programs can be forked.
//
// A running VM keeps a mutable working set for speed (pc, operand stack,
// variables, and dirty 4 KiB array pages). commit() writes it into the kernel's
// persistent Merkle store: arrays are Files, so unchanged pages stay shared.
// fork() = commit() + load(root): O(dirty pages) + O(1). The child reads pages
// lazily and copies a page only when it first writes to it. Execution is
// integer-only and single-threaded, so the same program from the same root
// produces the same output, forever. Every instruction costs one unit of gas.
//
//   let n = 0
//   let a = array(100000)
//   while n < len(a) { a[n] = n * n; n = n + 1 }
//   print a[99999]
//
// Values are int64 (wrapping). An array value is a tagged handle (bit 62) into
// the VM's array table, so a variable can hold either. hash(x) is splitmix64:
// the only source of "randomness", and itself deterministic.
#pragma once

#include "khonsu/store.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace khonsu::lune {

inline constexpr std::size_t kIntsPerPage = kPageSize / sizeof(std::int64_t);  // 512

enum class Op : std::uint8_t {
  Push, Load, Store, ALoad, AStore, NewArr, Len,
  Add, Sub, Mul, Div, Mod, Neg, Eq, Ne, Lt, Le, Gt, Ge, Not,
  Hash, Jmp, Jz, Print, PrintStr, Halt,
};

struct Instr {
  Op op;
  std::int64_t arg = 0;
  std::uint32_t line = 0;
};

struct Program {
  std::vector<Instr> code;
  std::vector<std::string> vars;  // slot -> name
  std::vector<std::string> strings;  // print "..." literals
  std::string source;
};

struct CompileError {
  std::uint32_t line = 0;
  std::string message;
};

[[nodiscard]] std::optional<Program> compile(const std::string& source, CompileError* err);

enum class Status : std::uint8_t { Running, Halted, Error, OutOfGas };
[[nodiscard]] const char* status_name(Status s);

class Vm {
public:
  Vm(NodeStore& store, Program program, std::uint64_t gas_limit = 50'000'000);

  // Executes up to `max_steps` instructions (a time slice). Returns steps executed.
  std::uint64_t run(std::uint64_t max_steps);

  // Persists the working set; returns the root of the whole VM state.
  NodeRef commit();
  // Restores a VM from a committed root (lazily: array pages are read on demand).
  static std::optional<Vm> load(NodeStore& store, NodeRef root, std::string* error);
  // fork(): commit, then a new VM from the same root. The parent keeps running untouched.
  Vm fork();

  // Sets a variable by name (a "what if" edit on a fork). False if unknown or an array.
  bool set_var(const std::string& name, std::int64_t value);

  [[nodiscard]] Status status() const { return status_; }
  [[nodiscard]] const std::string& error() const { return error_; }
  [[nodiscard]] std::uint64_t steps() const { return steps_; }
  [[nodiscard]] std::uint64_t gas_left() const { return gas_limit_ > steps_ ? gas_limit_ - steps_ : 0; }
  [[nodiscard]] std::uint32_t line() const;
  [[nodiscard]] const std::vector<std::string>& output() const { return output_; }
  [[nodiscard]] const Program& program() const { return prog_; }
  [[nodiscard]] std::vector<std::pair<std::string, std::string>> variables() const;  // name -> rendering
  [[nodiscard]] NodeRef last_root() const { return root_; }
  // The root this VM was loaded or forked from (kEmpty for a fresh VM): the fork point.
  [[nodiscard]] NodeRef base_root() const { return base_; }

  // Page identity of an array's committed pages, now and at the fork point (the CoW page map).
  [[nodiscard]] std::vector<PageSlot> array_pages(std::size_t array_id) const;
  [[nodiscard]] std::vector<PageSlot> base_array_pages(std::size_t array_id) const;
  [[nodiscard]] std::size_t array_count() const { return arrays_.size(); }
  // Distinct pages shared with the fork point that this VM has since written (copied).
  [[nodiscard]] std::uint64_t pages_copied() const { return pages_copied_; }
  [[nodiscard]] std::int64_t array_length(std::size_t id) const { return arrays_[id].length; }
  [[nodiscard]] std::size_t dirty_pages() const;
  [[nodiscard]] std::uint64_t printed() const { return printed_; }  // lines ever printed

private:
  struct Array {
    std::int64_t length = 0;
    NodeRef file;  // committed pages
    NodeRef base;  // pages at the fork point
    std::unordered_map<std::uint64_t, std::array<std::int64_t, kIntsPerPage>> dirty;
    mutable std::unordered_map<std::uint64_t, std::array<std::int64_t, kIntsPerPage>> clean;  // read cache
  };
  std::int64_t* page(std::size_t id, std::uint64_t pi, bool write);
  bool check_index(std::int64_t h, std::int64_t i);
  void fail(std::string msg);
  void print(std::string line);
  [[nodiscard]] std::string render(std::int64_t v) const;
  void restore(NodeRef root);

  NodeStore* store_;
  Program prog_;
  std::uint64_t gas_limit_;
  std::uint64_t pc_ = 0, steps_ = 0, pages_copied_ = 0, printed_ = 0;
  std::vector<std::int64_t> stack_;
  std::vector<std::int64_t> slots_;
  std::vector<Array> arrays_;
  std::vector<std::string> output_;
  Status status_ = Status::Running;
  std::string error_;
  NodeRef root_, base_;
  std::unordered_set<std::uint64_t> touched_;  // (array << 40 | page) written since load/fork
  // One-entry page cache: most array loops stay on one page for 512 accesses.
  std::size_t cache_array_ = SIZE_MAX;
  std::uint64_t cache_page_ = 0;
  std::int64_t* cache_ptr_ = nullptr;
  bool cache_write_ = false;
};

}  // namespace khonsu::lune
