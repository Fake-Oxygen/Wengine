#pragma once

/**
 * @file
 * Module registry for the `logic` sub-shell: one CommandSpec per
 * session verb (help/back/add/list/wire/unwire/remove/ignore/use/tie/
 * untie/reset/solve/truth/optimize/draw/save/load/export/step/poke/
 * peek) plus the session identity constants. Verbs dispatch
 * identically one-shot (`logic <verb> ...`) and in-session (`<verb>`
 * at the `[logic]> ` prompt); session control (back/exit/quit) is
 * interpreted by Shell, never by verbs - except `back`, which is a
 * real verb (returns kExitSubShell) so completion and behavior stay
 * uniform. The workspace is one process-wide Netlist shared by every
 * verb (single-threaded CLI only; tests reset via
 * reset_logic_workspace). To add a verb: declare a spec below, append
 * it to LogicRegistry, and keep the registry-size static_assert in
 * tests/test_cli_args.cpp in sync. Consumers: cli/shell.hpp (session
 * push), the LogicSpec entry in cli/commands.hpp, and completion via
 * cli/subshell.hpp complete_verbs.
 */

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

/**
 * @brief Session name shown in diagnostics (e.g. `logic`).
 */
inline constexpr std::string_view kSessionName = "logic";
/**
 * @brief Prompt string of the logic session.
 */
inline constexpr std::string_view kSessionPrompt = "[logic]> ";
/**
 * @brief Hint line telling the user how to leave the session.
 */
inline constexpr std::string_view kSessionExitHint = "back - leave the logic session";

// Process-wide module workspace (defined in src/cli/commands/logic.cpp).
// Session and one-shot verbs share it line-by-line.
/**
 * @brief Access the process-wide logic workspace Netlist.
 * @return Reference to the module workspace (defined in
 *         src/cli/commands/logic.cpp).
 * @note Session and one-shot lines in the same process see the same
 *       workspace; separate OS processes share via `save`/`load`
 *       files. Single-threaded CLI only.
 */
::wengine::logic::Netlist &workspace();
/**
 * @brief Reset the workspace to the empty state (tests use this between
 *        cases).
 */
inline void reset_logic_workspace() { ::wengine::logic::reset_netlist(workspace()); }

/**
 * @brief `help` verb: list session verbs or show one verb's usage.
 */
struct LogicHelpSpec {
  /**
   * @brief Verb name (`help`), valid in-session and one-shot.
   */
  static constexpr std::string_view kName = "help";
  /**
   * @brief One-line description for session help listings.
   */
  static constexpr std::string_view kDescription = "Show logic-session commands";
  /**
   * @brief Usage text printed by `help help` and usage errors.
   */
  static constexpr std::string_view kUsage =
      "help [verb]   (inside the logic session, or `logic help [verb]` one-shot)\n"
      "  help         list session verbs\n"
      "  help back    show one verb's usage";

  /**
   * @brief Execute the `help` verb.
   * @param[in] args Cursor over the argument tokens after the verb.
   * @param[out] out Result, usage, and error text.
   * @return 0 (kExitOk) on success; 1 (kExitRuntime) on runtime/check
   *         failure; 2 (kExitUsage) on a usage error.
   */
  static int run(ArgCursor &args, std::ostream &out);
  /**
   * @brief Tab-complete `help` arguments (verb names, optionally with
   *        `logic` stripped in one-shot form).
   * @param[in] prior_args Tokens after the verb, before the partial.
   * @param[in] partial Partial token under the cursor.
   * @param[out] out Candidates (no trailing spaces).
   */
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

/**
 * @brief `back` verb: leave the logic session (a real verb so behavior
 *        and completion stay uniform with the session-exit words).
 */
struct LogicBackSpec {
  /**
   * @brief Verb name (`back`), valid in-session and one-shot.
   */
  static constexpr std::string_view kName = "back";
  /**
   * @brief One-line description for session help listings.
   */
  static constexpr std::string_view kDescription = "Leave the logic session";
  /**
   * @brief Usage text printed by `help back` and usage errors.
   */
  static constexpr std::string_view kUsage = "back   (no arguments; `exit`/`quit` also leave the session)";

  /**
   * @brief Execute the `back` verb (validate args, then pop).
   * @param[in] args Cursor over the argument tokens after the verb
   *                 (must be empty).
   * @param[out] out Error text on stray arguments.
   * @return kExitSubShell in-session (Shell pops the session); in a
   *         one-shot (`logic back`) a usage error with a hint instead -
   *         the code never escapes to the process boundary.
   */
  static int run(ArgCursor &args, std::ostream &out);
  /**
   * @brief Tab-complete `back` arguments (nothing to complete).
   * @param[in] prior_args Tokens after the verb, before the partial.
   * @param[in] partial Partial token under the cursor.
   * @param[out] out Candidates (no trailing spaces).
   */
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

/**
 * @brief `add` verb: add one gate to the workspace.
 */
struct LogicAddSpec {
  /**
   * @brief Verb name (`add`), valid in-session and one-shot.
   */
  static constexpr std::string_view kName = "add";
  /**
   * @brief One-line description for session help listings.
   */
  static constexpr std::string_view kDescription = "Add a gate to the workspace";
  /**
   * @brief Usage text printed by `help add` and usage errors (gate
   *        kinds, pin counts, and per-kind semantics).
   */
  static constexpr std::string_view kUsage =
      "add <KIND> [--inputs N]\n"
      "  add AND\n"
      "  add OR --inputs 3\n"
      "  add TBUF\n"
      "  add DFF\n"
      "  add DFF --inputs 5\n"
      "  add JK\n"
      "  add T\n"
      "  add SRLATCH\n"
      "Kinds: AND OR NAND NOR XOR XNOR NOT BUF TBUF DFF DLATCH JK T SRLATCH\n"
      "(case-insensitive).\n"
      "NOT/BUF take exactly 1 input; TBUF exactly 2 (pin 1 = data, pin 2 =\n"
      "enable, active-high; released enable drives high-Z); DLATCH exactly\n"
      "2 (pins D/EN); T exactly\n"
      "2 (pins T/CLK); JK exactly 3 (pins J/K/CLK); SRLATCH exactly 2 (pins\n"
      "S/R); DFF 2 (pins D/CLK, the default) or 5 (full control set\n"
      "D/CLK/RST/SET/EN); the rest 2..8 (default 2, NOT/BUF default 1).\n"
      "DFF priority: RST (async) > SET (async) > EN-hold (sync) > D;\n"
      "RST+SET -> Q=0. Unconnected RST/SET read 0 (inactive), EN reads 1\n"
      "(enabled).\n"
      "JK: J=0,K=0 hold; J=0,K=1 reset (Q=0); J=1,K=0 set (Q=1); J=1,K=1\n"
      "toggle. T: T=0 hold; T=1 toggle (Q=NOT Q). Evaluated at the clock\n"
      "edge from pre-edge Q. Unconnected J/K/T read 0 (hold).\n"
      "DLATCH: D (data) + EN (level enable, not a clock). SRLATCH: S (async\n"
      "set) + R (async reset).\n"
      "DLATCH: EN=1 transparent (Q follows D); EN=0 frozen (Q holds, D\n"
      "ignored). No clock advance required.\n"
      "SRLATCH: S=1,R=0 set (Q=1); S=0,R=1 reset (Q=0); S=0,R=0 hold.\n"
      "S=1,R=1 is illegal and refused with state unchanged.\n"
      "Level latches belong to no clock domain: advances never gate their\n"
      "Q. Unconnected D/EN/S/R read 0 (hold).";

  /**
   * @brief Execute the `add` verb.
   * @param[in] args Cursor over the argument tokens after the verb
   *                 (`<KIND>` positional plus `--inputs`).
   * @param[out] out Result, usage, and error text.
   * @return 0 (kExitOk) on success; 1 (kExitRuntime) on runtime/check
   *         failure; 2 (kExitUsage) on a usage error. Errors change no
   *         state.
   */
  static int run(ArgCursor &args, std::ostream &out);
  /**
   * @brief Tab-complete `add` arguments (gate kinds, `--inputs`).
   * @param[in] prior_args Tokens after the verb, before the partial.
   * @param[in] partial Partial token under the cursor.
   * @param[out] out Candidates (no trailing spaces).
   */
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

/**
 * @brief `list` verb: show gates, wires, and the derived block
 *        interface.
 */
struct LogicListSpec {
  /**
   * @brief Verb name (`list`), valid in-session and one-shot.
   */
  static constexpr std::string_view kName = "list";
  /**
   * @brief One-line description for session help listings.
   */
  static constexpr std::string_view kDescription = "Show gates, wires and the derived block interface";
  /**
   * @brief Usage text printed by `help list` and usage errors.
   */
  static constexpr std::string_view kUsage =
      "list [--wires] [--iface]\n"
      "  list\n"
      "  list --wires --iface\n"
      "Shows gates (NAME KIND inputs=N fanout=F) + wires (FROM.Y -> TO.i)\n"
      "+ derived block interface (one input per tie class, slot-order leaders)\n"
      "+ ignored pins + tie groups (A.1 = B.2 per class).\n"
      "With no flags all sections print; flags select sections (gates always print).";

  /**
   * @brief Execute the `list` verb.
   * @param[in] args Cursor over the argument tokens after the verb
   *                 (`--wires`, `--iface` section flags).
   * @param[out] out Listing sections.
   * @return 0 (kExitOk) on success; 2 (kExitUsage) on a usage error.
   */
  static int run(ArgCursor &args, std::ostream &out);
  /**
   * @brief Tab-complete `list` arguments (section flags).
   * @param[in] prior_args Tokens after the verb, before the partial.
   * @param[in] partial Partial token under the cursor.
   * @param[out] out Candidates (no trailing spaces).
   */
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

/**
 * @brief `wire` verb: connect one output pin to one input pin.
 */
struct LogicWireSpec {
  /**
   * @brief Verb name (`wire`), valid in-session and one-shot.
   */
  static constexpr std::string_view kName = "wire";
  /**
   * @brief One-line description for session help listings.
   */
  static constexpr std::string_view kDescription = "Wire an output pin to an input pin";
  /**
   * @brief Usage text printed by `help wire` and usage errors.
   */
  static constexpr std::string_view kUsage =
      "wire <FROM.out> <TO.in> [--shared]\n"
      "  wire AND1.Y NOT1.1\n"
      "  wire DFF1.Q AND1.1\n"
      "  wire DFF1.NQ AND1.2\n"
      "  wire AND1.Y BUS.1 --shared\n"
      "Single-driver: the destination input must be unwired; cycles are rejected.\n"
      "--shared marks the input as a shared (multi-driver) net and lands the\n"
      "driver (up to 4 attachments per input; extra wires past the cap, and\n"
      "unsharing while 2+ drivers stay attached, fail loudly).\n"
      "Sequential gates use named pins (DFF1.D, DFF1.CLK, DFF1.Q, DFF1.NQ;\n"
      "DFF1.RST, DFF1.SET, DFF1.EN on --inputs 5 flops; DLATCH1.D,\n"
      "DLATCH1.EN; JK1.J, JK1.K, JK1.CLK; T1.T, T1.CLK; SRLATCH1.S,\n"
      "SRLATCH1.R).";

  /**
   * @brief Execute the `wire` verb.
   * @param[in] args Cursor over the argument tokens after the verb
   *                 (`<FROM.out> <TO.in>` positionals).
   * @param[out] out Result, usage, and error text.
   * @return 0 (kExitOk) on success; 1 (kExitRuntime) on a runtime/check
   *         failure; 2 (kExitUsage) on a usage error. Single-driver and
   *         cycle rules reject loudly; errors change no state.
   */
  static int run(ArgCursor &args, std::ostream &out);
  /**
   * @brief Tab-complete `wire` arguments (pin names).
   * @param[in] prior_args Tokens after the verb, before the partial.
   * @param[in] partial Partial token under the cursor.
   * @param[out] out Candidates (no trailing spaces).
   */
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

/**
 * @brief `unwire` verb: detach the wire driving one input pin.
 */
struct LogicUnwireSpec {
  /**
   * @brief Verb name (`unwire`), valid in-session and one-shot.
   */
  static constexpr std::string_view kName = "unwire";
  /**
   * @brief One-line description for session help listings.
   */
  static constexpr std::string_view kDescription = "Detach the wire driving an input pin";
  /**
   * @brief Usage text printed by `help unwire` and usage errors.
   */
  static constexpr std::string_view kUsage =
      "unwire [FROM.out] <TO.i>\n"
      "  unwire NOT1.1\n"
      "  unwire AND1.Y NOT1.1\n"
      "Detaches the wire driving the destination input pin (unwired inputs fail loudly).\n"
      "With FROM.out, releases exactly that edge (a shared extra compacts, the\n"
      "primary promotes extra 0); without it, releases the primary driver.";

  /**
   * @brief Execute the `unwire` verb.
   * @param[in] args Cursor over the argument tokens after the verb
   *                 (`<TO.i>` positional).
   * @param[out] out Result, usage, and error text.
   * @return 0 (kExitOk) on success; 1 (kExitRuntime) on a runtime/check
   *         failure; 2 (kExitUsage) on a usage error.
   */
  static int run(ArgCursor &args, std::ostream &out);
  /**
   * @brief Tab-complete `unwire` arguments (input pin names).
   * @param[in] prior_args Tokens after the verb, before the partial.
   * @param[in] partial Partial token under the cursor.
   * @param[out] out Candidates (no trailing spaces).
   */
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

/**
 * @brief `unshare` verb: clear the shared mark on an input pin.
 */
struct LogicUnshareSpec {
  /**
   * @brief Verb name (`unshare`), valid in-session and one-shot.
   */
  static constexpr std::string_view kName = "unshare";
  /**
   * @brief One-line description for session help listings.
   */
  static constexpr std::string_view kDescription = "Clear the shared (multi-driver) mark on an input pin";
  /**
   * @brief Usage text printed by `help unshare` and usage errors.
   */
  static constexpr std::string_view kUsage =
      "unshare <TO.i>\n"
      "  unshare BUS.1\n"
      "Clears the shared mark on the destination input pin (fails loudly while\n"
      "2+ drivers stay attached — unwire extras first — and on unmarked inputs).";

  /**
   * @brief Execute the `unshare` verb.
   * @param[in] args Cursor over the argument tokens after the verb
   *                 (`<TO.i>` positional).
   * @param[out] out Result, usage, and error text.
   * @return 0 (kExitOk) on success; 2 (kExitUsage) on a usage error.
   */
  static int run(ArgCursor &args, std::ostream &out);
  /**
   * @brief Tab-complete `unshare` arguments (input pin names).
   * @param[in] prior_args Tokens after the verb, before the partial.
   * @param[in] partial Partial token under the cursor.
   * @param[out] out Candidates (no trailing spaces).
   */
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

/**
 * @brief `pull` verb: set the pull on a destination input pin (012).
 */
struct LogicPullSpec {
  /**
   * @brief Verb name (`pull`), valid in-session and one-shot.
   */
  static constexpr std::string_view kName = "pull";
  /**
   * @brief One-line description for session help listings.
   */
  static constexpr std::string_view kDescription = "Set the pull-up/down on an input pin";
  /**
   * @brief Usage text printed by `help pull` and usage errors.
   */
  static constexpr std::string_view kUsage =
      "pull <TO.i> <up|down>\n"
      "  pull BUS.1 up\n"
      "Sets the pull on the destination input pin (wired or dangling, but\n"
      "never tied, ignored, or an output): an all-floating net resolves to\n"
      "the idle level (up -> 1, down -> 0); driven nets ignore the pull.\n"
      "Re-setting the same level succeeds; a different level replaces it.";

  /**
   * @brief Execute the `pull` verb.
   * @param[in] args Cursor over the argument tokens after the verb
   *                 (`<TO.i>` plus `<up|down>` positionals).
   * @param[out] out Result, usage, and error text.
   * @return 0 (kExitOk) on success; 2 (kExitUsage) on a usage error.
   */
  static int run(ArgCursor &args, std::ostream &out);
  /**
   * @brief Tab-complete `pull` arguments (input pin names, then levels).
   * @param[in] prior_args Tokens after the verb, before the partial.
   * @param[in] partial Partial token under the cursor.
   * @param[out] out Candidates (no trailing spaces).
   */
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

/**
 * @brief `unpull` verb: clear the pull on a destination input pin (012).
 */
struct LogicUnpullSpec {
  /**
   * @brief Verb name (`unpull`), valid in-session and one-shot.
   */
  static constexpr std::string_view kName = "unpull";
  /**
   * @brief One-line description for session help listings.
   */
  static constexpr std::string_view kDescription = "Clear the pull on an input pin";
  /**
   * @brief Usage text printed by `help unpull` and usage errors.
   */
  static constexpr std::string_view kUsage =
      "unpull <TO.i>\n"
      "  unpull BUS.1\n"
      "Clears the pull on the destination input pin (fails loudly when no\n"
      "pull is set — mirroring `unshare`'s lifecycle refusals).";

  /**
   * @brief Execute the `unpull` verb.
   * @param[in] args Cursor over the argument tokens after the verb
   *                 (`<TO.i>` positional).
   * @param[out] out Result, usage, and error text.
   * @return 0 (kExitOk) on success; 2 (kExitUsage) on a usage error.
   */
  static int run(ArgCursor &args, std::ostream &out);
  /**
   * @brief Tab-complete `unpull` arguments (input pin names).
   * @param[in] prior_args Tokens after the verb, before the partial.
   * @param[in] partial Partial token under the cursor.
   * @param[out] out Candidates (no trailing spaces).
   */
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

/**
 * @brief `remove` verb: remove one gate from the workspace.
 */
struct LogicRemoveSpec {
  /**
   * @brief Verb name (`remove`), valid in-session and one-shot.
   */
  static constexpr std::string_view kName = "remove";
  /**
   * @brief One-line description for session help listings.
   */
  static constexpr std::string_view kDescription = "Remove a gate from the workspace";
  /**
   * @brief Usage text printed by `help remove` and usage errors.
   */
  static constexpr std::string_view kUsage =
      "remove <GATE>\n"
      "  remove AND1\n"
      "Detaches every wire to/from the gate; the slot recycles, the counter is never reused.";

  /**
   * @brief Execute the `remove` verb.
   * @param[in] args Cursor over the argument tokens after the verb
   *                 (`<GATE>` positional).
   * @param[out] out Result, usage, and error text.
   * @return 0 (kExitOk) on success; 1 (kExitRuntime) on a runtime/check
   *         failure; 2 (kExitUsage) on a usage error.
   */
  static int run(ArgCursor &args, std::ostream &out);
  /**
   * @brief Tab-complete `remove` arguments (gate names).
   * @param[in] prior_args Tokens after the verb, before the partial.
   * @param[in] partial Partial token under the cursor.
   * @param[out] out Candidates (no trailing spaces).
   */
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

/**
 * @brief `ignore` verb: exclude a dangling pin from the block interface.
 */
struct LogicIgnoreSpec {
  /**
   * @brief Verb name (`ignore`), valid in-session and one-shot.
   */
  static constexpr std::string_view kName = "ignore";
  /**
   * @brief One-line description for session help listings.
   */
  static constexpr std::string_view kDescription = "Exclude a dangling pin from the block interface";
  /**
   * @brief Usage text printed by `help ignore` and usage errors.
   */
  static constexpr std::string_view kUsage =
      "ignore <PIN>\n"
      "  ignore AND1.2\n"
      "  ignore BUF1.Y\n"
      "Dangling ignored inputs tie to 0 during eval; wired inputs ignore the mark. Idempotent.";

  /**
   * @brief Execute the `ignore` verb.
   * @param[in] args Cursor over the argument tokens after the verb
   *                 (`<PIN>` positional).
   * @param[out] out Result, usage, and error text.
   * @return 0 (kExitOk) on success; 1 (kExitRuntime) on a runtime/check
   *         failure; 2 (kExitUsage) on a usage error.
   */
  static int run(ArgCursor &args, std::ostream &out);
  /**
   * @brief Tab-complete `ignore` arguments (pin names).
   * @param[in] prior_args Tokens after the verb, before the partial.
   * @param[in] partial Partial token under the cursor.
   * @param[out] out Candidates (no trailing spaces).
   */
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

/**
 * @brief `use` verb: clear the ignore mark on one pin.
 */
struct LogicUseSpec {
  /**
   * @brief Verb name (`use`), valid in-session and one-shot.
   */
  static constexpr std::string_view kName = "use";
  /**
   * @brief One-line description for session help listings.
   */
  static constexpr std::string_view kDescription = "Clear the ignore mark on a pin";
  /**
   * @brief Usage text printed by `help use` and usage errors.
   */
  static constexpr std::string_view kUsage =
      "use <PIN>\n"
      "  use AND1.2\n"
      "Re-exposes a previously ignored pin to the derived block interface. Idempotent.";

  /**
   * @brief Execute the `use` verb.
   * @param[in] args Cursor over the argument tokens after the verb
   *                 (`<PIN>` positional).
   * @param[out] out Result, usage, and error text.
   * @return 0 (kExitOk) on success; 1 (kExitRuntime) on a runtime/check
   *         failure; 2 (kExitUsage) on a usage error.
   */
  static int run(ArgCursor &args, std::ostream &out);
  /**
   * @brief Tab-complete `use` arguments (pin names).
   * @param[in] prior_args Tokens after the verb, before the partial.
   * @param[in] partial Partial token under the cursor.
   * @param[out] out Candidates (no trailing spaces).
   */
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

/**
 * @brief `tie` verb: tie two dangling input pins to one block input.
 */
struct LogicTieSpec {
  /**
   * @brief Verb name (`tie`), valid in-session and one-shot.
   */
  static constexpr std::string_view kName = "tie";
  /**
   * @brief One-line description for session help listings.
   */
  static constexpr std::string_view kDescription = "Tie two dangling input pins to the same net";
  /**
   * @brief Usage text printed by `help tie` and usage errors.
   */
  static constexpr std::string_view kUsage =
      "tie <A.i> <B.j>\n"
      "  tie NAND1.1 NAND1.2\n"
      "Ties two dangling input pins to one block input (lowest slot/input\n"
      "is the canonical rep; interface counts one input per class; both\n"
      "pins share one value in solve/truth). Both pins must dangle and be\n"
      "non-ignored; wired/ignored/output pins fail loudly. Idempotent\n"
      "within a class. Use `untie` to split, `list` to show tie groups.";

  /**
   * @brief Execute the `tie` verb.
   * @param[in] args Cursor over the argument tokens after the verb
   *                 (`<A.i> <B.j>` positionals).
   * @param[out] out Result, usage, and error text.
   * @return 0 (kExitOk) on success; 1 (kExitRuntime) on a runtime/check
   *         failure; 2 (kExitUsage) on a usage error. Both pins must
   *         dangle and be non-ignored; errors change no state.
   */
  static int run(ArgCursor &args, std::ostream &out);
  /**
   * @brief Tab-complete `tie` arguments (dangling input pin names).
   * @param[in] prior_args Tokens after the verb, before the partial.
   * @param[in] partial Partial token under the cursor.
   * @param[out] out Candidates (no trailing spaces).
   */
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

/**
 * @brief `untie` verb: remove one input pin from its tie group.
 */
struct LogicUntieSpec {
  /**
   * @brief Verb name (`untie`), valid in-session and one-shot.
   */
  static constexpr std::string_view kName = "untie";
  /**
   * @brief One-line description for session help listings.
   */
  static constexpr std::string_view kDescription = "Remove one input pin from its tie group";
  /**
   * @brief Usage text printed by `help untie` and usage errors.
   */
  static constexpr std::string_view kUsage =
      "untie <PIN.i>\n"
      "  untie NAND1.1\n"
      "Removes one input pin from its tie class (remaining members stay\n"
      "tied around the lowest survivor; a 2-pin group dissolves). Untied,\n"
      "wired, ignored, or output pins fail loudly.";

  /**
   * @brief Execute the `untie` verb.
   * @param[in] args Cursor over the argument tokens after the verb
   *                 (`<PIN.i>` positional).
   * @param[out] out Result, usage, and error text.
   * @return 0 (kExitOk) on success; 1 (kExitRuntime) on a runtime/check
   *         failure; 2 (kExitUsage) on a usage error.
   */
  static int run(ArgCursor &args, std::ostream &out);
  /**
   * @brief Tab-complete `untie` arguments (tied input pin names).
   * @param[in] prior_args Tokens after the verb, before the partial.
   * @param[in] partial Partial token under the cursor.
   * @param[out] out Candidates (no trailing spaces).
   */
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

/**
 * @brief `reset` verb: clear the whole workspace.
 */
struct LogicResetSpec {
  /**
   * @brief Verb name (`reset`), valid in-session and one-shot.
   */
  static constexpr std::string_view kName = "reset";
  /**
   * @brief One-line description for session help listings.
   */
  static constexpr std::string_view kDescription = "Clear the whole workspace";
  /**
   * @brief Usage text printed by `help reset` and usage errors.
   */
  static constexpr std::string_view kUsage =
      "reset [--force]\n"
      "  reset\n"
      "Clears every gate and wire; per-kind counters restart at 1.";

  /**
   * @brief Execute the `reset` verb.
   * @param[in] args Cursor over the argument tokens after the verb
   *                 (`--force` flag).
   * @param[out] out Result, usage, and error text.
   * @return 0 (kExitOk) on success; 1 (kExitRuntime) on a runtime/check
   *         failure; 2 (kExitUsage) on a usage error.
   */
  static int run(ArgCursor &args, std::ostream &out);
  /**
   * @brief Tab-complete `reset` arguments (the `--force` flag).
   * @param[in] prior_args Tokens after the verb, before the partial.
   * @param[in] partial Partial token under the cursor.
   * @param[out] out Candidates (no trailing spaces).
   */
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

/**
 * @brief `solve` verb: evaluate one assignment over the block interface.
 */
struct LogicSolveSpec {
  /**
   * @brief Verb name (`solve`), valid in-session and one-shot.
   */
  static constexpr std::string_view kName = "solve";
  /**
   * @brief One-line description for session help listings.
   */
  static constexpr std::string_view kDescription = "Evaluate one assignment over the block interface";
  /**
   * @brief Usage text printed by `help solve` and usage errors.
   */
  static constexpr std::string_view kUsage =
      "solve [--in <bits>] [--from <pin>=<0|1> ...]\n"
      "  solve --in 01\n"
      "  solve --from AND1.1=1 --from AND1.2=0\n"
      "Single-assignment eval in derived interface order (slot order).\n"
      "Unset inputs default to 0 (the assumed vector is echoed). One <PIN>=<bit>\n"
      "line per block output.";

  /**
   * @brief Execute the `solve` verb.
   * @param[in] args Cursor over the argument tokens after the verb
   *                 (`--in <bits>`, repeatable `--from <pin>=<0|1>`).
   * @param[out] out Result, usage, and error text.
   * @return 0 (kExitOk) on success; 1 (kExitRuntime) on a runtime/check
   *         failure; 2 (kExitUsage) on a usage error.
   */
  static int run(ArgCursor &args, std::ostream &out);
  /**
   * @brief Tab-complete `solve` arguments (flags, block input names).
   * @param[in] prior_args Tokens after the verb, before the partial.
   * @param[in] partial Partial token under the cursor.
   * @param[out] out Candidates (no trailing spaces).
   */
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

/**
 * @brief `truth` verb: stream the full truth table (bit-exact vs solve).
 */
struct LogicTruthSpec {
  /**
   * @brief Verb name (`truth`), valid in-session and one-shot.
   */
  static constexpr std::string_view kName = "truth";
  /**
   * @brief One-line description for session help listings.
   */
  static constexpr std::string_view kDescription = "Stream the full truth table (bit-exact vs solve)";
  /**
   * @brief Usage text printed by `help truth` and usage errors.
   */
  static constexpr std::string_view kUsage =
      "truth [--csv <path>] [--limit N]\n"
      "  truth\n"
      "  truth --limit 8\n"
      "  truth --csv out.csv\n"
      "Rows in interface order, input pins then output pins. The terminal\n"
      "prints a pretty aligned table; --csv writes raw 0/1 CSV to a file.\n"
      "Default cap: block inputs <= 16 (65536 rows); larger needs explicit\n"
      "--limit/--csv, and >= 31 inputs always needs --limit.";

  /**
   * @brief Execute the `truth` verb.
   * @param[in] args Cursor over the argument tokens after the verb
   *                 (`--csv <path>`, `--limit N`).
   * @param[out] out Table text (terminal form) plus counts.
   * @return 0 (kExitOk) on success; 1 (kExitRuntime) on a runtime/check
   *         failure; 2 (kExitUsage) on a usage error.
   */
  static int run(ArgCursor &args, std::ostream &out);
  /**
   * @brief Tab-complete `truth` arguments (flags).
   * @param[in] prior_args Tokens after the verb, before the partial.
   * @param[in] partial Partial token under the cursor.
   * @param[out] out Candidates (no trailing spaces).
   */
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

/**
 * @brief `optimize` verb: simplify the workspace (fold + De Morgan +
 *        NAND/NOR-only).
 */
struct LogicOptimizeSpec {
  /**
   * @brief Verb name (`optimize`), valid in-session and one-shot.
   */
  static constexpr std::string_view kName = "optimize";
  /**
   * @brief One-line description for session help listings.
   */
  static constexpr std::string_view kDescription = "Simplify the workspace (fold + De Morgan + NAND/NOR-only)";
  /**
   * @brief Usage text printed by `help optimize` and usage errors.
   */
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

  /**
   * @brief Execute the `optimize` verb.
   * @param[in] args Cursor over the argument tokens after the verb
   *                 (`--level O0|O1|O2`, `--to nand|nor`).
   * @param[out] out Before/after gate counts plus the interface report.
   * @return 0 (kExitOk) on success; 2 (kExitUsage) on a usage error
   *         (e.g. `--to` without O2).
   */
  static int run(ArgCursor &args, std::ostream &out);
  /**
   * @brief Tab-complete `optimize` arguments (level/to values).
   * @param[in] prior_args Tokens after the verb, before the partial.
   * @param[in] partial Partial token under the cursor.
   * @param[out] out Candidates (no trailing spaces).
   */
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

/**
 * @brief `draw` verb: draw the layered schematic (ImGui canvas or BMP).
 */
struct LogicDrawSpec {
  /**
   * @brief Verb name (`draw`), valid in-session and one-shot.
   */
  static constexpr std::string_view kName = "draw";
  /**
   * @brief One-line description for session help listings.
   */
  static constexpr std::string_view kDescription = "Draw the layered schematic (ImGui canvas + BMP)";
  /**
   * @brief Usage text printed by `help draw` and usage errors.
   */
  static constexpr std::string_view kUsage =
      "draw [--out <file.bmp>]\n"
      "  draw\n"
      "  draw --out schematic.bmp\n"
      "Opens an interactive schematic canvas (Dear ImGui over SDL2:\n"
      "wheel zooms, middle-drag pans, F fits, R resets, Esc closes;\n"
      "ANSI gate symbols, Manhattan wires) or, with --out, writes a BMP\n"
      "file via SDL_SaveBMP (no display needed). Needs an SDL2 build;\n"
      "the window additionally needs the vendored imgui submodule.";

  /**
   * @brief Execute the `draw` verb.
   * @param[in] args Cursor over the argument tokens after the verb
   *                 (`--out <file.bmp>`).
   * @param[out] out Result, usage, and error text.
   * @return 0 (kExitOk) on success; 1 (kExitRuntime) when SDL2/imgui is
   *         missing (rebuild hint) or the window fails; 2 (kExitUsage)
   *         on a usage error.
   */
  static int run(ArgCursor &args, std::ostream &out);
  /**
   * @brief Tab-complete `draw` arguments (the `--out` flag).
   * @param[in] prior_args Tokens after the verb, before the partial.
   * @param[in] partial Partial token under the cursor.
   * @param[out] out Candidates (no trailing spaces).
   */
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

/**
 * @brief `save` verb: write the workspace as versioned YAML.
 */
struct LogicSaveSpec {
  /**
   * @brief Verb name (`save`), valid in-session and one-shot.
   */
  static constexpr std::string_view kName = "save";
  /**
   * @brief One-line description for session help listings.
   */
  static constexpr std::string_view kDescription = "Save the workspace as versioned YAML";
  /**
   * @brief Usage text printed by `help save` and usage errors.
   */
  static constexpr std::string_view kUsage =
      "save <file.yaml>\n"
      "  save work.yaml\n"
      "Writes the versioned workspace (version: 1, with flop inits and\n"
      "clock domains) in canonical form; round-trips byte-identically\n"
      "through `load`.";

  /**
   * @brief Execute the `save` verb.
   * @param[in] args Cursor over the argument tokens after the verb
   *                 (`<file.yaml>` positional).
   * @param[out] out Result, usage, and error text.
   * @return 0 (kExitOk) on success; 1 (kExitRuntime) on I/O failure;
   *         2 (kExitUsage) on a usage error.
   */
  static int run(ArgCursor &args, std::ostream &out);
  /**
   * @brief Tab-complete `save` arguments (nothing path-specific).
   * @param[in] prior_args Tokens after the verb, before the partial.
   * @param[in] partial Partial token under the cursor.
   * @param[out] out Candidates (no trailing spaces).
   */
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

/**
 * @brief `load` verb: load a workspace file (replacing the workspace).
 */
struct LogicLoadSpec {
  /**
   * @brief Verb name (`load`), valid in-session and one-shot.
   */
  static constexpr std::string_view kName = "load";
  /**
   * @brief One-line description for session help listings.
   */
  static constexpr std::string_view kDescription = "Load a workspace file (replaces the workspace)";
  /**
   * @brief Usage text printed by `help load` and usage errors.
   */
  static constexpr std::string_view kUsage =
      "load <file.yaml>\n"
      "  load work.yaml\n"
      "Replaces the workspace; accepts version 1 only (retired generations are\n"
      "refused loudly); unknown keys/sections are errors. Fails atomically\n"
      "(workspace and pool state are kept on error).";

  /**
   * @brief Execute the `load` verb.
   * @param[in] args Cursor over the argument tokens after the verb
   *                 (`<file.yaml>` positional).
   * @param[out] out Result, usage, and error text.
   * @return 0 (kExitOk) on success; 1 (kExitRuntime) on I/O/parse
   *         failure (atomic - the workspace is kept); 2 (kExitUsage) on
   *         a usage error.
   */
  static int run(ArgCursor &args, std::ostream &out);
  /**
   * @brief Tab-complete `load` arguments (nothing path-specific).
   * @param[in] prior_args Tokens after the verb, before the partial.
   * @param[in] partial Partial token under the cursor.
   * @param[out] out Candidates (no trailing spaces).
   */
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

/**
 * @brief `export` verb: write truth table + schema (+ optional BMP).
 */
struct LogicExportSpec {
  /**
   * @brief Verb name (`export`), valid in-session and one-shot.
   */
  static constexpr std::string_view kName = "export";
  /**
   * @brief One-line description for session help listings.
   */
  static constexpr std::string_view kDescription = "Export truth table + schema (+ optional BMP)";
  /**
   * @brief Usage text printed by `help export` and usage errors.
   */
  static constexpr std::string_view kUsage =
      "export --truth <t.csv> --schema <s.yaml> [--bmp <b.bmp>]\n"
      "  export --truth t.csv --schema s.yaml\n"
      "One command, three artifacts: truth table CSV + workspace schema YAML\n"
      "+ optional BMP schematic (SDL_SaveBMP, no display needed).\n"
      "Loud per-artifact counts; the schema re-imports via `load`.";

  /**
   * @brief Execute the `export` verb.
   * @param[in] args Cursor over the argument tokens after the verb
   *                 (`--truth`, `--schema`, optional `--bmp`).
   * @param[out] out Per-artifact result counts.
   * @return 0 (kExitOk) on success; 1 (kExitRuntime) on a per-artifact
   *         failure; 2 (kExitUsage) on a usage error.
   */
  static int run(ArgCursor &args, std::ostream &out);
  /**
   * @brief Tab-complete `export` arguments (artifact flags).
   * @param[in] prior_args Tokens after the verb, before the partial.
   * @param[in] partial Partial token under the cursor.
   * @param[out] out Candidates (no trailing spaces).
   */
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

/**
 * @brief `step` verb: advance clocks by N rising edges (sequential).
 */
struct LogicStepSpec {
  /**
   * @brief Verb name (`step`), valid in-session and one-shot.
   */
  static constexpr std::string_view kName = "step";
  /**
   * @brief One-line description for session help listings.
   */
  static constexpr std::string_view kDescription = "Advance clocks by N edges (sequential)";
  /**
   * @brief Usage text printed by `help step` and usage errors.
   */
  static constexpr std::string_view kUsage =
      "step [DOMAIN] [N] [--cycles N]\n"
      "  step\n"
      "  step 5\n"
      "  step BUF1.Y 3\n"
      "  step --cycles 100\n"
      "Advances one rising edge per count: globally with no DOMAIN, else\n"
      "only flops clocked by DOMAIN (dangling CLK pins form domain\n"
      "`clk`). Level latches (DLATCH/SRLATCH) belong to no domain:\n"
      "they settle outside advances and never gate on them. Reports the\n"
      "cycle count, stepped flop transitions, and settled block outputs.\n"
      "Zero count reports without stepping; errors change no state.";

  /**
   * @brief Execute the `step` verb.
   * @param[in] args Cursor over the argument tokens after the verb
   *                 (`[DOMAIN] [N]`, `--cycles N`).
   * @param[out] out Cycle count, flop transitions, settled outputs.
   * @return 0 (kExitOk) on success; 1 (kExitRuntime) on a runtime/check
   *         failure; 2 (kExitUsage) on a usage error.
   */
  static int run(ArgCursor &args, std::ostream &out);
  /**
   * @brief Tab-complete `step` arguments (clock domains, `--cycles`).
   * @param[in] prior_args Tokens after the verb, before the partial.
   * @param[in] partial Partial token under the cursor.
   * @param[out] out Candidates (no trailing spaces).
   */
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

/**
 * @brief `poke` verb: drive or release a block input (sequential
 *        stimulus).
 */
struct LogicPokeSpec {
  /**
   * @brief Verb name (`poke`), valid in-session and one-shot.
   */
  static constexpr std::string_view kName = "poke";
  /**
   * @brief One-line description for session help listings.
   */
  static constexpr std::string_view kDescription = "Drive or release a block input pin (sequential stimulus)";
  /**
   * @brief Usage text printed by `help poke` and usage errors.
   */
  static constexpr std::string_view kUsage =
      "poke <PIN>=<0|1> [--from <PIN>=<0|1> ...]\n"
      "poke <PIN>=release\n"
      "poke [--clear]\n"
      "  poke DFF1.D=1\n"
      "  poke --from AND1.1=1 --from AND1.2=0\n"
      "  poke DFF1.D=release\n"
      "Drives a dangling block input to 0/1 (sticky across steps) or\n"
      "releases it back to wired behavior. Bare `poke` lists the map;\n"
      "`--clear` drops every entry. Named seq pins work (DFF1.D, DFF1.RST,\n"
      "DFF1.SET, DFF1.EN on --inputs 5 flops; DLATCH1.D, DLATCH1.EN; JK1.J,\n"
      "JK1.K, JK1.CLK; T1.T, T1.CLK; SRLATCH1.S, SRLATCH1.R); numeric\n"
      "aliases too (DFF1.1).\n"
      "Poked RST/SET force Q at once (async); poked EN=0 holds Q across\n"
      "steps. Priority: RST > SET > EN-hold > D; RST+SET -> Q=0.\n"
      "Poked DLATCH.EN=1 tracks D at once (transparent); EN=0 freezes Q.\n"
      "Poked SRLATCH.S/R set/reset Q at once; S=1,R=1 is illegal and\n"
      "refused (drive reverted, Q unchanged).\n"
      "Errors change no state.";

  /**
   * @brief Execute the `poke` verb.
   * @param[in] args Cursor over the argument tokens after the verb
   *                 (`<PIN>=<0|1>`, `<PIN>=release`, `--from`, `--clear`).
   * @param[out] out The drive map or result, usage, and error text.
   * @return 0 (kExitOk) on success; 1 (kExitRuntime) on a runtime/check
   *         failure; 2 (kExitUsage) on a usage error. Errors change no
   *         state.
   */
  static int run(ArgCursor &args, std::ostream &out);
  /**
   * @brief Tab-complete `poke` arguments (block input pins, `--from`).
   * @param[in] prior_args Tokens after the verb, before the partial.
   * @param[in] partial Partial token under the cursor.
   * @param[out] out Candidates (no trailing spaces).
   */
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

/**
 * @brief `peek` verb: show flop Q state (sequential readback).
 */
struct LogicPeekSpec {
  /**
   * @brief Verb name (`peek`), valid in-session and one-shot.
   */
  static constexpr std::string_view kName = "peek";
  /**
   * @brief One-line description for session help listings.
   */
  static constexpr std::string_view kDescription = "Show flop Q state (sequential readback)";
  /**
   * @brief Usage text printed by `help peek` and usage errors.
   */
  static constexpr std::string_view kUsage =
      "peek [GATE]\n"
      "  peek\n"
      "  peek DFF1\n"
      "Prints Q and NQ with the stable flop serial, in slot order: all\n"
      "flops with no argument, one block with GATE. Empty workspace\n"
      "reports `no flops` successfully. Read-only: never mutates state.";

  /**
   * @brief Execute the `peek` verb (read-only).
   * @param[in] args Cursor over the argument tokens after the verb
   *                 (optional `[GATE]` positional).
   * @param[out] out Q/NQ per flop with stable serials, slot order.
   * @return 0 (kExitOk) on success; 2 (kExitUsage) on a usage error.
   *         Never mutates state; an empty workspace reports `no flops`
   *         successfully.
   */
  static int run(ArgCursor &args, std::ostream &out);
  /**
   * @brief Tab-complete `peek` arguments (flop gate names).
   * @param[in] prior_args Tokens after the verb, before the partial.
   * @param[in] partial Partial token under the cursor.
   * @param[out] out Candidates (no trailing spaces).
   */
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

// Append new verb specs here; names stay unique via the compile-time validation.
/**
 * @brief The compile-time verb registry of the logic session (size is
 *        pinned by the registry static_assert + tests).
 * @note Append new verb specs here; names stay unique via the
 *       compile-time validation in cli/command.hpp, and the
 *       registry-size static_assert in tests/test_cli_args.cpp must be
 *       kept in sync when this list grows.
 */
using LogicRegistry = Registry<LogicHelpSpec, LogicBackSpec, LogicAddSpec, LogicListSpec,
                               LogicWireSpec, LogicUnwireSpec, LogicUnshareSpec, LogicPullSpec, LogicUnpullSpec,
                               LogicRemoveSpec, LogicIgnoreSpec, LogicUseSpec, LogicTieSpec, LogicUntieSpec,
                               LogicResetSpec, LogicSolveSpec, LogicTruthSpec, LogicOptimizeSpec,
                               LogicDrawSpec, LogicSaveSpec, LogicLoadSpec,
                               LogicExportSpec, LogicStepSpec, LogicPokeSpec, LogicPeekSpec>;

} // namespace wengine::cli::logic
