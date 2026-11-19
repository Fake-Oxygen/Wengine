# CLI

`cli` owns the command-line machinery: `cli/args.hpp` provides the
generic parsing — a zero-copy tokenizer and `ArgCursor` over pre-split
tokens (`--key value`, `--key=value`, flags with `--no-` negation,
positionals) plus thin per-quantity forwarders (`parse_voltage`,
`parse_quantity_value`, ...) onto the suffix tables that `units/` and
`signal/types.hpp` own; this module holds no independent unit tables.
Commands are compile-time registered specs (`kName`, `kDescription`,
`kUsage`, `run`, optional `complete`) validated by `consteval` checks
(unique, non-empty, whitespace-free names), and `cli/ic_dispatch.hpp`
gives chip calculators two-level dispatch under the single `ic`
command. The GUI parses unit strings through this header-only
`args.hpp` with no `wengine_cli` link dependency. Layering: `cli ->
solvers, core` (see [Architecture](../dev/architecture.md)).

## Usage pattern

Header-only: compile with `g++ -std=c++20 -I include`, run, expect
`sim run steps=10 dt=0.01 amp=3.3 cur=0.002 refused=1` and exit
status 0. The last two checks show the single-table discipline:
`2mA` parses as current, and cross-dimension text (`2mA` as volts)
fails loudly instead of silently converting.

```cpp
#include "wengine/cli/args.hpp"
#include "wengine/signal/types.hpp"

#include <cstdint>
#include <cstdio>
#include <string_view>

int main() {
  std::string_view tokens[]{"sim", "run", "--steps", "10", "--dt=0.01"};
  wengine::cli::ArgCursor args({tokens, 5});
  const std::string_view cmd = *args.next_positional();  // "sim"
  const std::string_view mode = *args.next_positional(); // "run"
  const double steps = args.take_double_option({"--steps"}).value;
  const double dt = args.take_double_option({"--dt"}).value;
  const bool consumed_all = !args.has_remaining(); // stray args are errors

  // Suffix tables stay owned by units/; cli only forwards per quantity.
  const double amp = wengine::cli::parse_voltage("3.3V").value;
  const double cur = wengine::cli::parse_quantity_value(
      wengine::signal::QuantityId::kCurrent, "2mA").value; // 0.002 A
  const bool loud_refusal = !wengine::cli::parse_quantity_value(
      wengine::signal::QuantityId::kVoltage, "2mA").ok(); // volts? fail

  std::printf("%.*s %.*s steps=%.0f dt=%.2f amp=%.1f cur=%.3f refused=%d\n",
              static_cast<int>(cmd.size()), cmd.data(),
              static_cast<int>(mode.size()), mode.data(), steps, dt, amp, cur,
              loud_refusal ? 1 : 0);
  return (cmd == "sim" && mode == "run" && steps == 10.0 && dt == 0.01 &&
          consumed_all && amp == 3.3 && cur == 0.002 && loud_refusal) ? 0 : 1;
}
```

## Key entities

Exit codes are centralized, never invented at call sites: `usage_error`
returns 2, `runtime_error` returns 1, and `from_status` maps any
`core::Status` through `core::to_exit_code` (see `cli/status.hpp`).
Registration stays a single obvious entry: append a spec to a
`Registry<...>` list and keep the registry-size `static_assert`s
passing. The `CommandSpec` concept (rendered in the listing below)
pins the spec shape at compile time; `ic` registries validate the same
way, so hundreds of parts never become hundreds of commands.

```{doxygenclass} wengine::cli::ArgCursor
:project: wengine
```

```{doxygenfunction} wengine::cli::parse_quantity_value
:project: wengine
```

```{doxygenstruct} wengine::cli::CommandDef
:project: wengine
```

```{doxygenstruct} wengine::cli::Registry
:project: wengine
```

```{doxygenfunction} wengine::cli::complete_line
:project: wengine
```

```{doxygenclass} wengine::cli::Shell
:project: wengine
```

## Complete module listing

Everything public in the module, including the top-level command specs
(`cli::commands`), the IC calculators and two-level dispatch
(`cli::ic`), the logic verb specs (`cli::logic`, `cli::logic_draw`),
and the parsing internals (`cli::detail`). `Shell` and `complete_line`
are implemented in the `wengine_cli` library; the rest is header-only.

```{doxygennamespace} wengine::cli
:project: wengine
```
