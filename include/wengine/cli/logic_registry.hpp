#pragma once

// Module registry for the `logic` sub-shell (LOGIC-1: session entry +
// `help`/`back` session verbs; LOGIC-8: full v1 verb set).
//
// To add a verb: declare one CommandSpec below (kName, kDescription,
// kUsage, run, optional complete), append it to LogicRegistry, keep the
// registry-size static_assert in tests/test_cli_args.cpp in sync.
// Verbs dispatch identically one-shot (`logic <verb> ...`) and in-session
// (`<verb> ...` at the `[logic]> ` prompt); session control
// (`back`/`exit`/`quit`) is interpreted by Shell, never by verbs —
// except `back`, which is a real verb (validates args, returns
// kExitSubShell) so completion and behavior stay uniform.
//
// Workspace: one process-wide Netlist shared by every verb (session and
// one-shot lines in the same process see the same workspace; separate OS
// processes share via `save`/`load` files). Single-threaded CLI only;
// tests reset via reset_logic_workspace().

#include "wengine/cli/command.hpp"
#include "wengine/logic/netlist.hpp"

#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::logic {

inline constexpr std::string_view kSessionName = "logic";
inline constexpr std::string_view kSessionPrompt = "[logic]> ";
inline constexpr std::string_view kSessionExitHint = "back - leave the logic session";

// Process-wide module workspace (defined in src/cli/commands/logic.cpp).
// Session and one-shot verbs share it line-by-line.
::wengine::logic::Netlist &workspace();
inline void reset_logic_workspace() { ::wengine::logic::reset_netlist(workspace()); }

struct LogicHelpSpec {
  static constexpr std::string_view kName = "help";
  static constexpr std::string_view kDescription = "Show logic-session commands";
  static constexpr std::string_view kUsage =
      "help [verb]   (inside the logic session, or `logic help [verb]` one-shot)\n"
      "  help         list session verbs\n"
      "  help back    show one verb's usage";

  static int run(ArgCursor &args, std::ostream &out);
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

struct LogicBackSpec {
  static constexpr std::string_view kName = "back";
  static constexpr std::string_view kDescription = "Leave the logic session";
  static constexpr std::string_view kUsage = "back   (no arguments; `exit`/`quit` also leave the session)";

  static int run(ArgCursor &args, std::ostream &out);
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

struct LogicAddSpec {
  static constexpr std::string_view kName = "add";
  static constexpr std::string_view kDescription = "Add a gate to the workspace";
  static constexpr std::string_view kUsage =
      "add <KIND> [--inputs N]\n"
      "  add AND\n"
      "  add OR --inputs 3\n"
      "Kinds: AND OR NAND NOR XOR XNOR NOT BUF (case-insensitive).\n"
      "NOT/BUF take exactly 1 input; the rest 2..8 (default 2, NOT/BUF default 1).";

  static int run(ArgCursor &args, std::ostream &out);
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

struct LogicListSpec {
  static constexpr std::string_view kName = "list";
  static constexpr std::string_view kDescription = "Show gates, wires and the derived block interface";
  static constexpr std::string_view kUsage =
      "list [--wires] [--iface]\n"
      "  list\n"
      "  list --wires --iface\n"
      "Shows gates (NAME KIND inputs=N fanout=F) + wires (FROM.Y -> TO.i)\n"
      "+ derived block interface (dangling pins in slot order) + ignored pins.\n"
      "With no flags all sections print; flags select sections (gates always print).";

  static int run(ArgCursor &args, std::ostream &out);
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

struct LogicWireSpec {
  static constexpr std::string_view kName = "wire";
  static constexpr std::string_view kDescription = "Wire an output pin to an input pin";
  static constexpr std::string_view kUsage =
      "wire <FROM.Y> <TO.i>\n"
      "  wire AND1.Y NOT1.1\n"
      "Single-driver: the destination input must be unwired; cycles are rejected.";

  static int run(ArgCursor &args, std::ostream &out);
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

struct LogicUnwireSpec {
  static constexpr std::string_view kName = "unwire";
  static constexpr std::string_view kDescription = "Detach the wire driving an input pin";
  static constexpr std::string_view kUsage =
      "unwire <TO.i>\n"
      "  unwire NOT1.1\n"
      "Detaches the wire driving the destination input pin (unwired inputs fail loudly).";

  static int run(ArgCursor &args, std::ostream &out);
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

struct LogicRemoveSpec {
  static constexpr std::string_view kName = "remove";
  static constexpr std::string_view kDescription = "Remove a gate from the workspace";
  static constexpr std::string_view kUsage =
      "remove <GATE>\n"
      "  remove AND1\n"
      "Detaches every wire to/from the gate; the slot recycles, the counter is never reused.";

  static int run(ArgCursor &args, std::ostream &out);
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

struct LogicIgnoreSpec {
  static constexpr std::string_view kName = "ignore";
  static constexpr std::string_view kDescription = "Exclude a dangling pin from the block interface";
  static constexpr std::string_view kUsage =
      "ignore <PIN>\n"
      "  ignore AND1.2\n"
      "  ignore BUF1.Y\n"
      "Dangling ignored inputs tie to 0 during eval; wired inputs ignore the mark. Idempotent.";

  static int run(ArgCursor &args, std::ostream &out);
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

struct LogicUseSpec {
  static constexpr std::string_view kName = "use";
  static constexpr std::string_view kDescription = "Clear the ignore mark on a pin";
  static constexpr std::string_view kUsage =
      "use <PIN>\n"
      "  use AND1.2\n"
      "Re-exposes a previously ignored pin to the derived block interface. Idempotent.";

  static int run(ArgCursor &args, std::ostream &out);
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

struct LogicResetSpec {
  static constexpr std::string_view kName = "reset";
  static constexpr std::string_view kDescription = "Clear the whole workspace";
  static constexpr std::string_view kUsage =
      "reset [--force]\n"
      "  reset\n"
      "Clears every gate and wire; per-kind counters restart at 1.";

  static int run(ArgCursor &args, std::ostream &out);
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

struct LogicSolveSpec {
  static constexpr std::string_view kName = "solve";
  static constexpr std::string_view kDescription = "Evaluate one assignment over the block interface";
  static constexpr std::string_view kUsage =
      "solve [--in <bits>] [--from <pin>=<0|1> ...]\n"
      "  solve --in 01\n"
      "  solve --from AND1.1=1 --from AND1.2=0\n"
      "Single-assignment eval in derived interface order (slot order).\n"
      "Unset inputs default to 0 (the assumed vector is echoed). One <PIN>=<bit>\n"
      "line per block output.";

  static int run(ArgCursor &args, std::ostream &out);
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

struct LogicTruthSpec {
  static constexpr std::string_view kName = "truth";
  static constexpr std::string_view kDescription = "Stream the full truth table (bit-exact vs solve)";
  static constexpr std::string_view kUsage =
      "truth [--csv <path>] [--limit N]\n"
      "  truth\n"
      "  truth --limit 8\n"
      "  truth --csv out.csv\n"
      "Rows in interface order, input pins then output pins. The terminal\n"
      "prints a pretty aligned table; --csv writes raw 0/1 CSV to a file.\n"
      "Default cap: block inputs <= 16 (65536 rows); larger needs explicit\n"
      "--limit/--csv, and >= 31 inputs always needs --limit.";

  static int run(ArgCursor &args, std::ostream &out);
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

struct LogicOptimizeSpec {
  static constexpr std::string_view kName = "optimize";
  static constexpr std::string_view kDescription = "Simplify the workspace (fold + De Morgan + NAND/NOR-only)";
  static constexpr std::string_view kUsage =
      "optimize [--level O0|O1|O2] [--to nand|nor]\n"
      "  optimize\n"
      "  optimize --level O1\n"
      "  optimize --level O2 --to nor\n"
      "O0 folds constants + bypasses BUF/double-negation + DCE (never grows);\n"
      "O1 adds De Morgan bubble-push fusion; O2 converts fully to --to\n"
      "(default nand, only read at O2; --to without O2 is an error).\n"
      "The workspace is replaced; before/after gate counts print, plus a\n"
      "loud interface report when pins change (folding drops dead pins,\n"
      "O2 duplicates pins for inversion: compare truth tables under pin\n"
      "replication, not row-for-row).";

  static int run(ArgCursor &args, std::ostream &out);
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

struct LogicDrawSpec {
  static constexpr std::string_view kName = "draw";
  static constexpr std::string_view kDescription = "Draw the layered schematic (ASCII + optional BMP)";
  static constexpr std::string_view kUsage =
      "draw [--out <file.bmp>]\n"
      "  draw\n"
      "  draw --out schematic.bmp\n"
      "Prints the layered ASCII schematic (one column per topo level) and,\n"
      "with --out, writes a BMP file (hand-rolled writer, no display needed).";

  static int run(ArgCursor &args, std::ostream &out);
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

struct LogicSaveSpec {
  static constexpr std::string_view kName = "save";
  static constexpr std::string_view kDescription = "Save the workspace as versioned YAML";
  static constexpr std::string_view kUsage =
      "save <file.yaml>\n"
      "  save work.yaml\n"
      "Writes the versioned workspace (version: 1) in canonical form; round-trips\n"
      "byte-identically through `load`.";

  static int run(ArgCursor &args, std::ostream &out);
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

struct LogicLoadSpec {
  static constexpr std::string_view kName = "load";
  static constexpr std::string_view kDescription = "Load a workspace file (replaces the workspace)";
  static constexpr std::string_view kUsage =
      "load <file.yaml>\n"
      "  load work.yaml\n"
      "Replaces the workspace; version must be 1 (loud otherwise); unknown\n"
      "keys/sections are errors. Fails atomically (the workspace is kept on error).";

  static int run(ArgCursor &args, std::ostream &out);
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

struct LogicExportSpec {
  static constexpr std::string_view kName = "export";
  static constexpr std::string_view kDescription = "Export truth table + schema (+ optional BMP)";
  static constexpr std::string_view kUsage =
      "export --truth <t.csv> --schema <s.yaml> [--bmp <b.bmp>]\n"
      "  export --truth t.csv --schema s.yaml\n"
      "One command, three artifacts: truth table CSV + workspace schema YAML\n"
      "+ optional BMP schematic. Loud per-artifact counts.";

  static int run(ArgCursor &args, std::ostream &out);
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

// Append new verb specs here; names stay unique via the compile-time validation.
using LogicRegistry = Registry<LogicHelpSpec, LogicBackSpec, LogicAddSpec, LogicListSpec,
                               LogicWireSpec, LogicUnwireSpec, LogicRemoveSpec,
                               LogicIgnoreSpec, LogicUseSpec, LogicResetSpec,
                               LogicSolveSpec, LogicTruthSpec, LogicOptimizeSpec,
                               LogicDrawSpec, LogicSaveSpec, LogicLoadSpec,
                               LogicExportSpec>;

} // namespace wengine::cli::logic
