#include "check.hpp"
#include "khonsu/lune.hpp"

#include <string>
#include <vector>

using namespace khonsu;
using namespace khonsu::lune;

namespace {

const char* kSieve = R"(# primes below n, then a checksum
let n = 20000
let sieve = array(n)
let i = 2
let count = 0
while i < n {
  if sieve[i] == 0 {
    count = count + 1
    let j = i * i
    while j < n { sieve[j] = 1; j = j + i }
  }
  i = i + 1
}
print "primes"
print count
)";

const char* kWalk = R"(let seed = 7
let steps = 5000
let a = array(65536)
let x = seed
let t = 0
while t < steps {
  x = hash(x + t)
  a[x % len(a)] = a[x % len(a)] + 1
  t = t + 1
}
let sum = 0
let k = 0
while k < len(a) { sum = sum + a[k] * (k + 1); k = k + 1 }
print sum
)";

Program must_compile(const char* src) {
  CompileError e;
  auto p = compile(src, &e);
  if (!p) std::printf("    compile error line %u: %s\n", e.line, e.message.c_str());
  return p ? std::move(*p) : Program{};
}

std::vector<std::string> run_to_end(Vm& vm) {
  while (vm.status() == Status::Running) vm.run(1'000'000);
  return vm.output();
}

}  // namespace

TEST("lune: runs a program to completion") {
  NodeStore s;
  Vm vm(s, must_compile(kSieve));
  const auto out = run_to_end(vm);
  CHECK(vm.status() == Status::Halted);
  REQUIRE(out.size() == 2);
  CHECK(out[0] == "primes");
  CHECK(out[1] == "2262");
}

TEST("lune: compile errors carry the line") {
  CompileError e;
  CHECK(!compile("let x = 1\nprint y\n", &e));
  CHECK(e.line == 2);
  CHECK(e.message.find("unknown variable 'y'") != std::string::npos);
  CHECK(!compile("let x = (1 + 2\n", &e));
  CHECK(!compile("while 1 { print 1\n", &e));
  CHECK(!compile("let x = 1 $ 2", &e));
  CHECK(!compile("print foo(1)", &e));
  CHECK(compile("if 1 { print 1 } else if 0 { print 2 } else { print 3 }", &e).has_value());
}

TEST("lune: runtime errors stop at the faulting line") {
  NodeStore s;
  Vm a(s, must_compile("let a = array(3)\nprint a[3]\n"));
  run_to_end(a);
  CHECK(a.status() == Status::Error);
  CHECK(a.line() == 2);
  CHECK(a.error().find("out of bounds") != std::string::npos);
  Vm b(s, must_compile("let z = 0\nprint 1 / z\n"));
  run_to_end(b);
  CHECK(b.status() == Status::Error);
  CHECK(b.error() == "division by zero");
  Vm c(s, must_compile("while 1 { }"), 1000);
  run_to_end(c);
  CHECK(c.status() == Status::OutOfGas);
  CHECK(c.steps() == 1000);
}

TEST("lune: fork at any step reproduces the uninterrupted run") {
  NodeStore s;
  Vm ref(s, must_compile(kWalk));
  const auto expected = run_to_end(ref);
  const std::uint64_t total = ref.steps();
  REQUIRE(expected.size() == 1);
  Xoshiro256 rng(42);
  for (int trial = 0; trial < 12; ++trial) {
    Vm vm(s, must_compile(kWalk));
    vm.run(rng.below(total));
    Vm child = vm.fork();
    CHECK(child.steps() == vm.steps());
    CHECK(run_to_end(child) == expected);
    CHECK(run_to_end(vm) == expected);  // the parent is untouched by its child
    CHECK(child.steps() == total);
  }
}

TEST("lune: load(root) resumes a committed state") {
  NodeStore s;
  Vm vm(s, must_compile(kSieve));
  vm.run(123'457);
  const NodeRef root = vm.commit();
  std::string err;
  auto back = Vm::load(s, root, &err);
  REQUIRE(back.has_value());
  CHECK(back->steps() == 123'457);
  CHECK(run_to_end(*back) == run_to_end(vm));
  CHECK(!Vm::load(s, kEmpty, &err));
}

TEST("lune: a what-if edit diverges only the fork") {
  NodeStore s;
  Vm vm(s, must_compile(kWalk));
  vm.run(40);  // the lets have run; the loop has not
  Vm child = vm.fork();
  CHECK(child.set_var("x", 99));
  CHECK(!child.set_var("a", 1));  // arrays are not numbers
  CHECK(!child.set_var("nope", 1));
  const auto p = run_to_end(vm), c = run_to_end(child);
  CHECK(p != c);
  Vm again(s, must_compile(kWalk));
  CHECK(run_to_end(again) == p);
}

TEST("lune: fork shares every page; a write copies exactly one") {
  NodeStore s;
  Vm vm(s, must_compile("let a = array(100000)\nlet i = 0\nwhile i < len(a) { a[i] = i; i = i + 1 }\n"
                        "a[70000] = 7\nprint a[70000]\n"));
  while (vm.status() == Status::Running && vm.line() < 4) vm.run(1);
  REQUIRE(vm.line() == 4);
  Vm child = vm.fork();
  const auto before = child.array_pages(0);
  CHECK(before.size() == (100000 * 8 + kPageSize - 1) / kPageSize);
  CHECK(child.pages_copied() == 0);
  const auto st0 = s.stats().pages_allocated;
  run_to_end(child);
  CHECK(child.output() == std::vector<std::string>{"7"});
  CHECK(child.pages_copied() == 1);
  child.commit();
  const auto after = child.array_pages(0);
  REQUIRE(after.size() == before.size());
  std::size_t differing = 0;
  for (std::size_t i = 0; i < after.size(); ++i) differing += after[i].page != before[i].page;
  CHECK(differing == 1);
  CHECK(s.stats().pages_allocated - st0 == 1);
  CHECK(child.base_root() == vm.last_root());
  CHECK(child.base_array_pages(0).size() == before.size());
  // the parent still sees its own value
  vm.commit();
  CHECK(vm.array_pages(0)[70000 * 8 / kPageSize].page == before[70000 * 8 / kPageSize].page);
}

TEST("lune: garbage collection keeps committed states") {
  NodeStore s;
  Vm vm(s, must_compile(kSieve));
  vm.run(50'000);
  Vm child = vm.fork();
  vm.run(10'000);
  const NodeRef roots[] = {vm.commit(), child.commit()};
  s.collect(roots);
  CHECK(run_to_end(child) == run_to_end(vm));
}
