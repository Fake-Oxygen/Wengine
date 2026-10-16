#include "wengine/cli/command_registry.hpp"
#include "wengine/cli/commands.hpp"
#include "wengine/cli/complete.hpp"
#include "wengine/cli/ic_dispatch.hpp"
#include "wengine/cli/ic_registry.hpp"
#include "wengine/cli/shell.hpp"
#include "wengine/cli/status.hpp"

#include <array>
#include <cassert>
#include <iostream>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace wengine::cli;

namespace {

// Test-only chips exercising the shared two-level dispatch. Never
// registered in the production IcRegistry.
struct DemoOpsSpec {
  static constexpr std::string_view kName = "demo";
  static constexpr std::string_view kDescription = "demo chip (test only)";
  static constexpr std::string_view kUsage =
      "ic demo <op> [options]\n"
      "  ic demo info\n"
      "  ic demo double --x N\n";

  static int run(ArgCursor &args, std::ostream &out) {
    if (args.take_flag({"--help", "-h"})) {
      out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
      return wengine::core::kExitOk;
    }
    std::optional<std::string_view> op = args.next_positional();
    if (!op) {
      return usage_error(out, "missing op (info|double)", kUsage);
    }
    if (*op == "info") {
      if (args.has_remaining()) {
        return unexpected_error(out, *args.first_remaining(), kUsage);
      }
      out << "demo info ok\n";
      return wengine::core::kExitOk;
    }
    if (*op == "double") {
      auto o = args.take_option({"--x"});
      if (!o) {
        return usage_error(out, "double needs --x", kUsage);
      }
      auto p = parse_double(*o);
      if (!p.ok()) {
        return usage_error(out, "invalid --x", kUsage);
      }
      if (args.has_remaining()) {
        return unexpected_error(out, *args.first_remaining(), kUsage);
      }
      out << "2*x = " << (p.value * 2.0) << '\n';
      return wengine::core::kExitOk;
    }
    return usage_error(out, "unknown op (info|double)", kUsage);
  }

  static void complete(std::span<const std::string_view> prior_args, std::string_view partial,
                       std::vector<std::string> &out) {
    if (!partial.empty() && partial.front() == '-') {
      constexpr std::string_view kX = "--x";
      if (kX.substr(0, partial.size()) == partial) {
        out.emplace_back(kX);
      }
      return;
    }
    if (prior_args.empty()) {
      constexpr std::string_view kOps[] = {"info", "double"};
      for (std::string_view op : kOps) {
        if (op.substr(0, partial.size()) == partial) {
          out.emplace_back(op);
        }
      }
    }
  }
};

struct PlainSpec {
  static constexpr std::string_view kName = "plain";
  static constexpr std::string_view kDescription = "chip without completion (test only)";
  static constexpr std::string_view kUsage = "ic plain\n";

  static int run(ArgCursor &args, std::ostream &out) {
    if (args.has_remaining()) {
      return unexpected_error(out, *args.first_remaining(), kUsage);
    }
    out << "plain ok\n";
    return wengine::core::kExitOk;
  }
  // No complete member: delegation must stay silent, never crash.
};

using TestChips = Registry<DemoOpsSpec, PlainSpec>;

constexpr std::string_view kTopUsage = "ic <chip> <op> [options]\n";

std::pair<int, std::string> run_test_chips(std::string line) {
  std::array<std::string_view, kMaxArgs> buf{};
  const std::size_t n = tokenize_into(line, buf);
  const std::size_t m = n < buf.size() ? n : buf.size();
  std::span<const std::string_view> args{buf.data(), m};
  ArgCursor cur{args};
  std::ostringstream out;
  const int rc = ic::dispatch<TestChips>(cur, out, "ic", "IC test", kTopUsage);
  return {rc, out.str()};
}

std::pair<int, std::string> run_production(std::string line) {
  std::array<std::string_view, kMaxArgs> buf{};
  const std::size_t n = tokenize_into(line, buf);
  const std::size_t m = n < buf.size() ? n : buf.size();
  std::span<const std::string_view> args{buf.data(), m};
  ArgCursor cur{args};
  std::ostringstream out;
  const int rc = commands::IcSpec::run(cur, out);
  return {rc, out.str()};
}

void test_dispatch_routes() {
  static_assert(TestChips::find("demo") != nullptr);
  static_assert(TestChips::find("plain") != nullptr);
  static_assert(TestChips::find("nope") == nullptr);
  static_assert(TestChips::all().size() == 2);

  {
    auto [rc, s] = run_test_chips("demo info");
    assert(rc == 0 && s.find("demo info ok") != std::string::npos);
  }
  {
    auto [rc, s] = run_test_chips("demo double --x 21");
    assert(rc == 0 && s.find("42") != std::string::npos);
  }
  {
    auto [rc, s] = run_test_chips("plain");
    assert(rc == 0 && s.find("plain ok") != std::string::npos);
  }
  {
    // Per-chip help: the trailing --help is forwarded, not consumed up top.
    auto [rc, s] = run_test_chips("demo --help");
    assert(rc == 0 && s.find("ic demo <op>") != std::string::npos);
  }
  {
    auto [rc, s] = run_test_chips("");
    assert(rc == 2 && s.find("missing chip") != std::string::npos);
  }
  {
    auto [rc, s] = run_test_chips("--help");
    assert(rc == 0 && s.find("Available ICs") != std::string::npos);
    assert(s.find("demo") != std::string::npos && s.find("plain") != std::string::npos);
  }
  {
    auto [rc, s] = run_test_chips("nope info");
    assert(rc == 2 && s.find("unknown IC") != std::string::npos);
    assert(s.find("nope") != std::string::npos);
  }
  {
    auto [rc, s] = run_test_chips("demo bogus");
    assert(rc == 2 && s.find("unknown op") != std::string::npos);
  }
  {
    auto [rc, s] = run_test_chips("demo");
    assert(rc == 2 && s.find("missing op") != std::string::npos);
  }
  {
    // Stray tokens surface from the chip layer, not silently ignored.
    auto [rc, s] = run_test_chips("demo info extra");
    assert(rc == 2 && s.find("unexpected argument") != std::string::npos);
  }
  {
    auto [rc, s] = run_test_chips("demo double --x 2 --bogus");
    assert(rc == 2 && s.find("unexpected argument") != std::string::npos);
  }
  {
    auto [rc, s] = run_test_chips("--bogus");
    assert(rc == 2 && s.find("unexpected argument") != std::string::npos);
  }
}

void test_dispatch_complete() {
  {
    std::vector<std::string> out;
    ic::complete<TestChips>({}, "", out);
    const std::vector<std::string> want{"demo", "plain"};
    assert(out == want);
  }
  {
    std::vector<std::string> out;
    ic::complete<TestChips>({}, "d", out);
    const std::vector<std::string> want{"demo"};
    assert(out == want);
  }
  {
    // Delegation strips the chip token: the chip sees an empty prior.
    std::vector<std::string> out;
    const std::array<std::string_view, 1> prior{"demo"};
    ic::complete<TestChips>(prior, "", out);
    const std::vector<std::string> want{"info", "double"};
    assert(out == want);
  }
  {
    std::vector<std::string> out;
    const std::array<std::string_view, 1> prior{"demo"};
    ic::complete<TestChips>(prior, "in", out);
    const std::vector<std::string> want{"info"};
    assert(out == want);
  }
  {
    // Chip without a complete member: silent, never a crash.
    std::vector<std::string> out;
    const std::array<std::string_view, 1> prior{"plain"};
    ic::complete<TestChips>(prior, "", out);
    assert(out.empty());
  }
  {
    std::vector<std::string> out;
    const std::array<std::string_view, 1> prior{"nope"};
    ic::complete<TestChips>(prior, "", out);
    assert(out.empty());
  }
  {
    std::vector<std::string> out;
    ic::complete<TestChips>({}, "--", out);
    const std::vector<std::string> want{"--help"};
    assert(out == want);
  }
}

void test_production_empty_registry() {
  static_assert(IcRegistry::all().empty());
  static_assert(IcRegistry::find("demo") == nullptr);
  static_assert(CliRegistry::find("ic") != nullptr);
  static_assert(CliRegistry::find("ic")->complete != nullptr);

  {
    auto [rc, s] = run_production("");
    assert(rc == 2 && s.find("missing chip") != std::string::npos);
  }
  {
    auto [rc, s] = run_production("--help");
    assert(rc == 0 && s.find("Available ICs") != std::string::npos);
    assert(s.find("(none registered yet)") != std::string::npos);
  }
  {
    auto [rc, s] = run_production("ne555");
    assert(rc == 2 && s.find("unknown IC") != std::string::npos);
  }
  {
    // Empty registry completes to nothing (options still complete).
    std::vector<std::string> out;
    commands::IcSpec::complete({}, "", out);
    assert(out.empty());
    commands::IcSpec::complete({}, "--", out);
    const std::vector<std::string> want{"--help"};
    assert(out == want);
  }
  {
    // End-to-end through the generic completion engine + shell.
    const auto all = CliRegistry::all();
    assert(complete_line("ic ", 3, all).empty());
    Shell sh;
    sh.set_history_file("");
    std::ostringstream out;
    assert(sh.execute("ic --help", out) == 0);
    assert(out.str().find("Available ICs") != std::string::npos);
    assert(sh.execute("help ic", out) == 0);
    assert(sh.execute("ic nope", out) == 2);
  }
}

} // namespace

int main() {
  test_dispatch_routes();
  test_dispatch_complete();
  test_production_empty_registry();
  std::cout << "test_ic: OK\n";
  return 0;
}
