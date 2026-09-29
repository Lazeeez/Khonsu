#include "khonsu/lune.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace khonsu::lune {

namespace {

constexpr std::int64_t kArrayTag = std::int64_t{1} << 62;
constexpr std::int64_t kMaxArrayLen = std::int64_t{1} << 24;  // 128 MiB of int64
constexpr std::size_t kMaxArrays = 4096;
constexpr std::size_t kMaxOutput = 2000;  // lines kept (the oldest drop off)

bool is_array(std::int64_t v) { return (v & ~std::int64_t{0x7fffffff}) == kArrayTag; }
std::int64_t array_id(std::int64_t v) { return v & 0x7fffffff; }

std::int64_t wrap_add(std::int64_t a, std::int64_t b) {
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b));
}
std::int64_t wrap_sub(std::int64_t a, std::int64_t b) {
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b));
}
std::int64_t wrap_mul(std::int64_t a, std::int64_t b) {
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) * static_cast<std::uint64_t>(b));
}
std::int64_t splitmix(std::int64_t x) {
  std::uint64_t z = static_cast<std::uint64_t>(x) + 0x9e3779b97f4a7c15ull;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return static_cast<std::int64_t>((z ^ (z >> 31)) >> 1);  // non-negative
}

// ── lexer ─────────────────────────────────────────────────────────────────────────────

enum class T : std::uint8_t { Int, Ident, Str, Sym, Sep, End };

struct Token {
  T kind;
  std::string text;
  std::int64_t value = 0;
  std::uint32_t line = 1;
};

struct Fail {
  std::uint32_t line;
  std::string message;
};

std::vector<Token> lex(const std::string& src) {
  std::vector<Token> out;
  std::uint32_t line = 1;
  std::size_t i = 0;
  auto is_id = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
  while (i < src.size()) {
    const char c = src[i];
    if (c == '\n' || c == ';') {
      out.push_back({T::Sep, std::string(1, c), 0, line});
      if (c == '\n') ++line;
      ++i;
    } else if (c == ' ' || c == '\t' || c == '\r') {
      ++i;
    } else if (c == '#') {
      while (i < src.size() && src[i] != '\n') ++i;
    } else if (std::isdigit(static_cast<unsigned char>(c))) {
      std::size_t j = i;
      while (j < src.size() && (std::isdigit(static_cast<unsigned char>(src[j])) || src[j] == '_')) ++j;
      std::string digits;
      for (std::size_t k = i; k < j; ++k)
        if (src[k] != '_') digits += src[k];
      std::int64_t v = 0;
      const auto r = std::from_chars(digits.data(), digits.data() + digits.size(), v);
      if (r.ec != std::errc{}) throw Fail{line, "number too large: " + digits};
      out.push_back({T::Int, digits, v, line});
      i = j;
    } else if (is_id(c)) {
      std::size_t j = i;
      while (j < src.size() && is_id(src[j])) ++j;
      out.push_back({T::Ident, src.substr(i, j - i), 0, line});
      i = j;
    } else if (c == '"') {
      std::size_t j = i + 1;
      std::string s;
      while (j < src.size() && src[j] != '"' && src[j] != '\n') s += src[j++];
      if (j >= src.size() || src[j] != '"') throw Fail{line, "unterminated string"};
      out.push_back({T::Str, s, 0, line});
      i = j + 1;
    } else {
      static const char* two[] = {"==", "!=", "<=", ">=", "&&", "||"};
      std::string sym(1, c);
      for (const char* t : two)
        if (src.compare(i, 2, t) == 0) sym = t;
      if (sym.size() == 1 && std::strchr("+-*/%<>=!()[]{},", c) == nullptr)
        throw Fail{line, std::string("unexpected character '") + c + "'"};
      out.push_back({T::Sym, sym, 0, line});
      i += sym.size();
    }
  }
  out.push_back({T::End, "", 0, line});
  return out;
}

// ── parser / code generator ───────────────────────────────────────────────────────────

class Compiler {
public:
  explicit Compiler(std::vector<Token> toks) : t_(std::move(toks)) {}

  Program run(std::string source) {
    stmts(/*in_block=*/false);
    emit(Op::Halt);
    prog_.source = std::move(source);
    return std::move(prog_);
  }

private:
  const Token& peek() const { return t_[p_]; }
  bool sym(const char* s) const { return peek().kind == T::Sym && peek().text == s; }
  bool word(const char* s) const { return peek().kind == T::Ident && peek().text == s; }
  [[noreturn]] void fail(const std::string& m) const { throw Fail{peek().line, m}; }
  void expect(const char* s) {
    if (!sym(s)) fail(std::string("expected '") + s + "'" + (peek().text.empty() ? "" : " near '" + peek().text + "'"));
    ++p_;
  }
  std::size_t emit(Op op, std::int64_t arg = 0) {
    prog_.code.push_back({op, arg, line_});
    return prog_.code.size() - 1;
  }
  std::size_t here() const { return prog_.code.size(); }
  void patch(std::size_t at, std::size_t target) { prog_.code[at].arg = static_cast<std::int64_t>(target); }

  std::int64_t slot(const std::string& name, bool declare) {
    const auto it = slots_.find(name);
    if (it != slots_.end()) return it->second;
    if (!declare) fail("unknown variable '" + name + "' (declare it with let)");
    const auto s = static_cast<std::int64_t>(prog_.vars.size());
    prog_.vars.push_back(name);
    slots_.emplace(name, s);
    return s;
  }

  static bool keyword(const std::string& s) {
    for (const char* k : {"let", "while", "if", "else", "print", "halt", "and", "or", "not", "true", "false"})
      if (s == k) return true;
    return false;
  }

  void seps() {
    while (peek().kind == T::Sep) ++p_;
  }

  void stmts(bool in_block) {
    seps();
    while (peek().kind != T::End && !(in_block && sym("}"))) {
      stmt();
      if (peek().kind == T::Sep) {
        seps();
      } else if (!(in_block && sym("}")) && peek().kind != T::End) {
        fail("expected a new line or ';' near '" + peek().text + "'");
      }
    }
  }

  void block() {
    expect("{");
    stmts(/*in_block=*/true);
    expect("}");
  }

  void stmt() {
    line_ = peek().line;
    const Token& tk = peek();
    if (tk.kind != T::Ident) fail("expected a statement near '" + tk.text + "'");
    if (tk.text == "let") {
      ++p_;
      if (peek().kind != T::Ident || keyword(peek().text)) fail("expected a variable name after let");
      const std::string name = peek().text;
      ++p_;
      expect("=");
      expr();
      emit(Op::Store, slot(name, true));  // declared after the initialiser: `let x = x` is an error
    } else if (tk.text == "while") {
      ++p_;
      const std::size_t top = here();
      expr();
      const std::size_t jz = emit(Op::Jz);
      block();
      emit(Op::Jmp, static_cast<std::int64_t>(top));
      patch(jz, here());
    } else if (tk.text == "if") {
      if_stmt();
    } else if (tk.text == "print") {
      ++p_;
      if (peek().kind == T::Str) {
        prog_.strings.push_back(peek().text);
        ++p_;
        emit(Op::PrintStr, static_cast<std::int64_t>(prog_.strings.size() - 1));
      } else {
        expr();
        emit(Op::Print);
      }
    } else if (tk.text == "halt") {
      ++p_;
      emit(Op::Halt);
    } else if (keyword(tk.text)) {
      fail("unexpected '" + tk.text + "'");
    } else {
      const std::string name = tk.text;
      ++p_;
      if (sym("[")) {
        emit(Op::Load, slot(name, false));
        ++p_;
        expr();
        expect("]");
        expect("=");
        expr();
        emit(Op::AStore);
      } else {
        expect("=");
        expr();
        emit(Op::Store, slot(name, false));
      }
    }
  }

  void if_stmt() {
    ++p_;  // if
    expr();
    const std::size_t jz = emit(Op::Jz);
    block();
    std::size_t save = p_;
    seps();
    if (word("else")) {
      ++p_;
      const std::size_t jend = emit(Op::Jmp);
      patch(jz, here());
      if (word("if")) {
        if_stmt();
      } else {
        block();
      }
      patch(jend, here());
    } else {
      p_ = save;
      patch(jz, here());
    }
  }

  void truthy() {
    emit(Op::Push, 0);
    emit(Op::Ne);
  }

  void expr() { or_expr(); }
  void or_expr() {
    and_expr();
    while (word("or") || sym("||")) {
      ++p_;
      truthy();
      and_expr();
      truthy();
      emit(Op::Add);
      truthy();
    }
  }
  void and_expr() {
    cmp_expr();
    while (word("and") || sym("&&")) {
      ++p_;
      truthy();
      cmp_expr();
      truthy();
      emit(Op::Mul);
    }
  }
  void cmp_expr() {
    add_expr();
    for (;;) {
      Op op;
      if (sym("==")) op = Op::Eq;
      else if (sym("!=")) op = Op::Ne;
      else if (sym("<")) op = Op::Lt;
      else if (sym("<=")) op = Op::Le;
      else if (sym(">")) op = Op::Gt;
      else if (sym(">=")) op = Op::Ge;
      else return;
      ++p_;
      add_expr();
      emit(op);
    }
  }
  void add_expr() {
    mul_expr();
    while (sym("+") || sym("-")) {
      const Op op = sym("+") ? Op::Add : Op::Sub;
      ++p_;
      mul_expr();
      emit(op);
    }
  }
  void mul_expr() {
    unary();
    while (sym("*") || sym("/") || sym("%")) {
      const Op op = sym("*") ? Op::Mul : sym("/") ? Op::Div : Op::Mod;
      ++p_;
      unary();
      emit(op);
    }
  }
  void unary() {
    if (sym("-")) {
      ++p_;
      unary();
      emit(Op::Neg);
    } else if (sym("!") || word("not")) {
      ++p_;
      unary();
      emit(Op::Not);
    } else {
      postfix();
    }
  }
  void postfix() {
    primary();
    while (sym("[")) {
      ++p_;
      expr();
      expect("]");
      emit(Op::ALoad);
    }
  }
  void primary() {
    const Token tk = peek();
    if (tk.kind == T::Int) {
      ++p_;
      emit(Op::Push, tk.value);
    } else if (sym("(")) {
      ++p_;
      expr();
      expect(")");
    } else if (tk.kind == T::Ident && (tk.text == "true" || tk.text == "false")) {
      ++p_;
      emit(Op::Push, tk.text == "true" ? 1 : 0);
    } else if (tk.kind == T::Ident && !keyword(tk.text)) {
      ++p_;
      if (sym("(")) {
        ++p_;
        Op op;
        if (tk.text == "array") op = Op::NewArr;
        else if (tk.text == "len") op = Op::Len;
        else if (tk.text == "hash") op = Op::Hash;
        else throw Fail{tk.line, "unknown function '" + tk.text + "' (have array, len, hash)"};
        expr();
        expect(")");
        emit(op);
      } else {
        emit(Op::Load, slot(tk.text, false));
      }
    } else {
      fail(tk.kind == T::End ? "unexpected end of program" : "unexpected '" + tk.text + "'");
    }
  }

  std::vector<Token> t_;
  std::size_t p_ = 0;
  std::uint32_t line_ = 1;
  Program prog_;
  std::unordered_map<std::string, std::int64_t> slots_;
};

// ── state encoding ───────────────────────────────────────────────────────────────────

std::string ints_blob(const std::vector<std::int64_t>& v) {
  std::string s(v.size() * sizeof(std::int64_t), '\0');
  if (!v.empty()) std::memcpy(s.data(), v.data(), s.size());
  return s;
}

std::vector<std::int64_t> blob_ints(std::string_view s) {
  std::vector<std::int64_t> v(s.size() / sizeof(std::int64_t));
  if (!v.empty()) std::memcpy(v.data(), s.data(), v.size() * sizeof(std::int64_t));
  return v;
}

std::uint64_t num(const NodeStore& st, NodeRef root, std::string_view key) {
  const auto t = st.get_text(root, key);
  std::uint64_t v = 0;
  if (t) std::from_chars(t->data(), t->data() + t->size(), v);
  return v;
}

}  // namespace

std::optional<Program> compile(const std::string& source, CompileError* err) {
  try {
    return Compiler(lex(source)).run(source);
  } catch (const Fail& f) {
    if (err) *err = {f.line, f.message};
    return std::nullopt;
  }
}

const char* status_name(Status s) {
  switch (s) {
    case Status::Running: return "running";
    case Status::Halted: return "halted";
    case Status::Error: return "error";
    case Status::OutOfGas: return "out of gas";
  }
  return "?";
}

Vm::Vm(NodeStore& store, Program program, std::uint64_t gas_limit)
    : store_(&store), prog_(std::move(program)), gas_limit_(gas_limit), slots_(prog_.vars.size(), 0) {}

void Vm::fail(std::string msg) {
  status_ = Status::Error;
  error_ = std::move(msg);
}

std::uint32_t Vm::line() const {
  if (prog_.code.empty()) return 0;
  return prog_.code[static_cast<std::size_t>(std::min<std::uint64_t>(pc_, prog_.code.size() - 1))].line;
}

// Page access. Reads go dirty -> clean cache -> store; the first write to a page
// moves it into the dirty set (a copy-on-write copy if the page was committed).
std::int64_t* Vm::page(std::size_t id, std::uint64_t pi, bool write) {
  if (id == cache_array_ && pi == cache_page_ && (cache_write_ || !write)) return cache_ptr_;
  Array& a = arrays_[id];
  std::int64_t* p;
  if (auto d = a.dirty.find(pi); d != a.dirty.end()) {
    p = d->second.data();
    write = true;
  } else if (!write) {
    auto c = a.clean.find(pi);
    if (c == a.clean.end()) {
      c = a.clean.try_emplace(pi).first;
      store_->read_page(a.file, pi, reinterpret_cast<std::byte*>(c->second.data()));
    }
    p = c->second.data();
  } else {
    auto& slot = a.dirty[pi];
    if (auto node = a.clean.extract(pi)) slot = node.mapped();
    else store_->read_page(a.file, pi, reinterpret_cast<std::byte*>(slot.data()));
    if (touched_.insert(static_cast<std::uint64_t>(id) << 40 | pi).second && !store_->page_at(a.base, pi).empty())
      ++pages_copied_;
    p = slot.data();
  }
  cache_array_ = id;
  cache_page_ = pi;
  cache_ptr_ = p;
  cache_write_ = write;
  return p;
}

bool Vm::check_index(std::int64_t h, std::int64_t i) {
  if (!is_array(h) || static_cast<std::size_t>(array_id(h)) >= arrays_.size()) {
    fail("indexing a number, not an array");
    return false;
  }
  const std::int64_t n = arrays_[static_cast<std::size_t>(array_id(h))].length;
  if (i < 0 || i >= n) {
    fail("index " + std::to_string(i) + " out of bounds for array of length " + std::to_string(n));
    return false;
  }
  return true;
}

std::uint64_t Vm::run(std::uint64_t max_steps) {
  if (status_ != Status::Running) return 0;
  const std::uint64_t start = steps_;
  const Instr* code = prog_.code.data();
  auto pop = [&] {
    const std::int64_t v = stack_.back();
    stack_.pop_back();
    return v;
  };
  const std::uint64_t budget = std::min(max_steps, gas_left());
  const std::uint64_t stop = steps_ + budget;
  while (steps_ < stop) {
    const Instr& in = code[pc_];
    ++steps_;
    ++pc_;
    switch (in.op) {
      case Op::Push: stack_.push_back(in.arg); break;
      case Op::Load: stack_.push_back(slots_[static_cast<std::size_t>(in.arg)]); break;
      case Op::Store: slots_[static_cast<std::size_t>(in.arg)] = pop(); break;
      case Op::ALoad: {
        const std::int64_t i = pop(), h = pop();
        if (!check_index(h, i)) break;
        const auto u = static_cast<std::uint64_t>(i);
        stack_.push_back(page(static_cast<std::size_t>(array_id(h)), u / kIntsPerPage, false)[u % kIntsPerPage]);
        break;
      }
      case Op::AStore: {
        const std::int64_t v = pop(), i = pop(), h = pop();
        if (!check_index(h, i)) break;
        const auto u = static_cast<std::uint64_t>(i);
        page(static_cast<std::size_t>(array_id(h)), u / kIntsPerPage, true)[u % kIntsPerPage] = v;
        break;
      }
      case Op::NewArr: {
        const std::int64_t n = pop();
        if (n < 0 || n > kMaxArrayLen) {
          fail("array(" + std::to_string(n) + "): length must be 0.." + std::to_string(kMaxArrayLen));
          break;
        }
        if (arrays_.size() >= kMaxArrays) {
          fail("too many arrays (max " + std::to_string(kMaxArrays) + ")");
          break;
        }
        arrays_.emplace_back().length = n;
        stack_.push_back(kArrayTag | static_cast<std::int64_t>(arrays_.size() - 1));
        break;
      }
      case Op::Len: {
        const std::int64_t h = pop();
        if (!is_array(h) || static_cast<std::size_t>(array_id(h)) >= arrays_.size()) {
          fail("len() of a number");
          break;
        }
        stack_.push_back(arrays_[static_cast<std::size_t>(array_id(h))].length);
        break;
      }
      case Op::Add: { const std::int64_t b = pop(); stack_.back() = wrap_add(stack_.back(), b); break; }
      case Op::Sub: { const std::int64_t b = pop(); stack_.back() = wrap_sub(stack_.back(), b); break; }
      case Op::Mul: { const std::int64_t b = pop(); stack_.back() = wrap_mul(stack_.back(), b); break; }
      case Op::Div:
      case Op::Mod: {
        const std::int64_t b = pop(), a = pop();
        if (b == 0) {
          fail(in.op == Op::Div ? "division by zero" : "modulo by zero");
          break;
        }
        if (a == std::numeric_limits<std::int64_t>::min() && b == -1) stack_.push_back(in.op == Op::Div ? a : 0);
        else stack_.push_back(in.op == Op::Div ? a / b : a % b);
        break;
      }
      case Op::Neg: stack_.back() = wrap_sub(0, stack_.back()); break;
      case Op::Not: stack_.back() = stack_.back() == 0 ? 1 : 0; break;
      case Op::Eq: { const std::int64_t b = pop(); stack_.back() = stack_.back() == b; break; }
      case Op::Ne: { const std::int64_t b = pop(); stack_.back() = stack_.back() != b; break; }
      case Op::Lt: { const std::int64_t b = pop(); stack_.back() = stack_.back() < b; break; }
      case Op::Le: { const std::int64_t b = pop(); stack_.back() = stack_.back() <= b; break; }
      case Op::Gt: { const std::int64_t b = pop(); stack_.back() = stack_.back() > b; break; }
      case Op::Ge: { const std::int64_t b = pop(); stack_.back() = stack_.back() >= b; break; }
      case Op::Hash: stack_.back() = splitmix(stack_.back()); break;
      case Op::Jmp: pc_ = static_cast<std::uint64_t>(in.arg); break;
      case Op::Jz:
        if (pop() == 0) pc_ = static_cast<std::uint64_t>(in.arg);
        break;
      case Op::Print: print(render(pop())); break;
      case Op::PrintStr: print(prog_.strings[static_cast<std::size_t>(in.arg)]); break;
      case Op::Halt:
        --pc_;
        status_ = Status::Halted;
        break;
    }
    if (status_ != Status::Running) {
      if (status_ == Status::Error) --pc_;  // point at the faulting instruction
      break;
    }
  }
  if (status_ == Status::Running && gas_left() == 0) status_ = Status::OutOfGas;
  return steps_ - start;
}

void Vm::print(std::string line) {
  output_.push_back(std::move(line));
  ++printed_;
  if (output_.size() > kMaxOutput) output_.erase(output_.begin(), output_.begin() + (output_.size() - kMaxOutput));
}

std::string Vm::render(std::int64_t v) const {
  if (is_array(v) && static_cast<std::size_t>(array_id(v)) < arrays_.size())
    return "array(" + std::to_string(arrays_[static_cast<std::size_t>(array_id(v))].length) + ")";
  return std::to_string(v);
}

NodeRef Vm::commit() {
  NodeStore& st = *store_;
  NodeRef r = root_;
  r = st.put_text(r, "src", prog_.source);
  r = st.put_text(r, "pc", std::to_string(pc_));
  r = st.put_text(r, "steps", std::to_string(steps_));
  r = st.put_text(r, "gas", std::to_string(gas_limit_));
  r = st.put_text(r, "status", std::to_string(static_cast<int>(status_)));
  r = st.put_text(r, "error", error_);
  r = st.put_text(r, "stack", ints_blob(stack_));
  r = st.put_text(r, "vars", ints_blob(slots_));
  r = st.put_text(r, "printed", std::to_string(printed_));
  std::string out;
  for (const auto& l : output_) (out += l) += '\n';
  r = st.put_text(r, "out", out);
  r = st.put_text(r, "arrays", std::to_string(arrays_.size()));
  for (std::size_t id = 0; id < arrays_.size(); ++id) {
    Array& a = arrays_[id];
    const auto bytes = static_cast<std::uint64_t>(a.length) * sizeof(std::int64_t);
    if (a.dirty.empty() && !a.file.empty()) continue;
    NodeRef f = a.file.empty() ? st.empty_file() : a.file;
    for (auto& [pi, data] : a.dirty) {
      const std::uint64_t off = pi * kPageSize;
      const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(kPageSize, bytes - off));
      f = st.file_write(f, off, {reinterpret_cast<const std::byte*>(data.data()), n});
    }
    if (st.file_size(f) != bytes) f = st.file_truncate(f, bytes);
    for (auto& [pi, data] : a.dirty) a.clean.insert_or_assign(pi, data);
    a.dirty.clear();
    a.file = f;
    r = st.put(r, "a/" + std::to_string(id), f);
  }
  cache_array_ = SIZE_MAX;
  root_ = r;
  return r;
}

void Vm::restore(NodeRef root) {
  const NodeStore& st = *store_;
  root_ = base_ = root;
  pc_ = num(st, root, "pc");
  steps_ = num(st, root, "steps");
  gas_limit_ = num(st, root, "gas");
  status_ = static_cast<Status>(num(st, root, "status"));
  error_ = std::string(st.get_text(root, "error").value_or(""));
  stack_ = blob_ints(st.get_text(root, "stack").value_or(""));
  slots_ = blob_ints(st.get_text(root, "vars").value_or(""));
  printed_ = num(st, root, "printed");
  output_.clear();
  const std::string_view out = st.get_text(root, "out").value_or("");
  for (std::size_t i = 0; i < out.size();) {
    const std::size_t j = out.find('\n', i);
    output_.emplace_back(out.substr(i, j - i));
    i = j + 1;
  }
  arrays_.clear();
  arrays_.resize(static_cast<std::size_t>(num(st, root, "arrays")));
  for (std::size_t id = 0; id < arrays_.size(); ++id) {
    arrays_[id].file = arrays_[id].base = st.get(root, "a/" + std::to_string(id));
    arrays_[id].length = static_cast<std::int64_t>(st.file_size(arrays_[id].file) / sizeof(std::int64_t));
  }
  cache_array_ = SIZE_MAX;
  pages_copied_ = 0;
  touched_.clear();
}

std::optional<Vm> Vm::load(NodeStore& store, NodeRef root, std::string* error) {
  const auto src = store.get_text(root, "src");
  if (!src) {
    if (error) *error = "not a Lune state root";
    return std::nullopt;
  }
  CompileError ce;
  auto prog = compile(std::string(*src), &ce);
  if (!prog) {
    if (error) *error = "stored program does not compile: " + ce.message;
    return std::nullopt;
  }
  Vm vm(store, std::move(*prog));
  vm.restore(root);
  if (vm.slots_.size() != vm.prog_.vars.size() || vm.pc_ >= vm.prog_.code.size()) {
    if (error) *error = "state does not match its program";
    return std::nullopt;
  }
  return vm;
}

Vm Vm::fork() {
  const NodeRef r = commit();
  Vm child(*store_, prog_);
  child.restore(r);
  return child;
}

bool Vm::set_var(const std::string& name, std::int64_t value) {
  for (std::size_t i = 0; i < prog_.vars.size(); ++i) {
    if (prog_.vars[i] != name) continue;
    if (is_array(slots_[i])) return false;
    slots_[i] = value;
    return true;
  }
  return false;
}

std::vector<std::pair<std::string, std::string>> Vm::variables() const {
  std::vector<std::pair<std::string, std::string>> out;
  for (std::size_t i = 0; i < prog_.vars.size(); ++i) out.emplace_back(prog_.vars[i], render(slots_[i]));
  return out;
}

std::size_t Vm::dirty_pages() const {
  std::size_t n = 0;
  for (const auto& a : arrays_) n += a.dirty.size();
  return n;
}

std::vector<PageSlot> Vm::array_pages(std::size_t id) const {
  if (id >= arrays_.size()) return {};
  return store_->file_pages(arrays_[id].file);
}

std::vector<PageSlot> Vm::base_array_pages(std::size_t id) const {
  if (id >= arrays_.size()) return {};
  return store_->file_pages(arrays_[id].base);
}

}  // namespace khonsu::lune
