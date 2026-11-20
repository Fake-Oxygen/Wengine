// 013-delay-annotation-verbs: propagation-delay annotation story tests.
//
// Engine seam (exact whole-ps parse + field write/clear) through CLI
// verbs to YAML round-trip. Links wengine_cli + wengine_logic_yaml to
// prove the seam-to-verb-to-artifact chain.
//
// Reference anchoring (constitution I/III, FR-014): every conversion
// golden below is hand-derived decimal arithmetic from
// contracts/engine-seam.md (1.5ns -> 1500 by 1.5 x 10^9 ps, never the
// implementation's readback); CLI representation, saved artifact, and
// re-read artifact are all pinned, including every refusal class and
// the zero-value/cleared equivalence.

#include "wengine/logic/block_interface.hpp"
#include "wengine/logic/eval.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/seq_state.hpp"
#include "wengine/logic/types.hpp"
#include "wengine/logic/yaml_store.hpp"
#include "wengine/world/serialize.hpp"
#include "wengine/world/state.hpp"

#include "wengine/cli/args.hpp"
#include "wengine/cli/logic_registry.hpp"
#include "wengine/cli/shell.hpp"
#include "wengine/tui/exec.hpp"

#include <cassert>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace wengine;

namespace {

constexpr std::uint64_t kU64Max = 0xFFFFFFFFFFFFFFFFULL;

bool contains(std::string_view hay, std::string_view needle) {
  return hay.find(needle) != std::string_view::npos;
}

// Run one session line through the process-wide workspace Shell.
int run_line(const std::string &line, std::string &text) {
  wengine::cli::Shell sh;
  sh.set_history_file("");
  std::ostringstream out;
  const int rc = sh.execute(line, out);
  text = out.str();
  return rc;
}

// US1 (T005): value-exactness goldens against the hand-derived table in
// contracts/engine-seam.md (never the implementation's readback).
void test_parse_time_ps_goldens() {
  using wengine::cli::parse_time_ps;
  // Hand-derived: 1.5 x 10^9 ps = 1500; 10^6/10^3 = 1; 2 x 10^6;
  // 5 x 10^9; 1.5 x 3600 x 10^12 = 5.4 x 10^15.
  assert(parse_time_ps("1500").value == 1500);
  assert(parse_time_ps("1.5ns").value == 1500);
  assert(parse_time_ps("250ps").value == 250);
  assert(parse_time_ps("0.001ns").value == 1);
  assert(parse_time_ps("2us").value == 2000000);
  assert(parse_time_ps("5ms").value == 5000000000ULL);
  assert(parse_time_ps("1.5h").value == 5400000000000000ULL);
  assert(parse_time_ps("1min").value == 60000000000000ULL);
  assert(parse_time_ps("2sec").value == 2000000000000ULL);
  assert(parse_time_ps("1.5s").value == 1500000000000ULL);
  assert(parse_time_ps("0").value == 0);
  assert(parse_time_ps("0001500").value == 1500);
  // Full field range: bare u64 max is exact.
  assert(parse_time_ps("18446744073709551615").value == kU64Max);
  // Same bound through a suffixed spelling (18446744.073709551615 s).
  assert(parse_time_ps("18446744.073709551615s").value == kU64Max);
  // Every refusal class from the contract, each with its message.
  assert(!parse_time_ps("-1").ok());
  assert(contains(parse_time_ps("-1").error, "non-negative"));
  assert(!parse_time_ps("1.5ps").ok()); // sub-picosecond
  assert(contains(parse_time_ps("1.5ps").error, "not a whole number of picoseconds"));
  assert(!parse_time_ps("0.5ps").ok());
  assert(!parse_time_ps("6000h").ok()); // 2.16e19 > u64 max
  assert(contains(parse_time_ps("6000h").error, "exceeds the 64-bit picosecond field"));
  assert(!parse_time_ps("18446744073709551616").ok()); // bare u64 max + 1
  assert(!parse_time_ps("1.5").ok()); // bare form is integer ps
  assert(contains(parse_time_ps("1.5").error, "bare form is integer picoseconds"));
  assert(!parse_time_ps("1500V").ok()); // cross-dimension suffix
  assert(contains(parse_time_ps("1500V").error, "unknown time suffix"));
  assert(!parse_time_ps("5fortnights").ok());
  assert(!parse_time_ps("1NS").ok()); // case-sensitive spellings
  assert(!parse_time_ps("1e3ns").ok()); // exponent grammar
  assert(contains(parse_time_ps("1e3ns").error, "bad delay value"));
  assert(!parse_time_ps("").ok());
  assert(!parse_time_ps("ns").ok());
  assert(!parse_time_ps(".5ns").ok());
  assert(!parse_time_ps("5.ns").ok());
  assert(!parse_time_ps("1.5.2ns").ok());
  // A 20-digit fraction that is whole only past the picosecond grid
  // refuses (long-digit proof path, not truncation).
  assert(!parse_time_ps("10.000000000000000000001ns").ok());
}

// US1 (T005): the engine seam directly — live-slot validation, the
// zero-field clear refusal, zero-as-legal-write, every-kind legality.
void test_gate_delay_seam() {
  logic::Netlist nl{};
  const auto and_slot = logic::add_gate(nl, logic::GateKind::kAnd, 2);
  assert(and_slot.ok());
  const auto dff_slot = logic::add_gate(nl, logic::GateKind::kDff);
  assert(dff_slot.ok());
  // Dead slot refuses, netlist untouched.
  assert(!logic::set_gate_delay(nl, 500, 1500).ok());
  assert(!logic::clear_gate_delay(nl, 500).ok());
  // Clear on a zero field refuses loudly (the clear_pull precedent).
  assert(!logic::clear_gate_delay(nl, and_slot.value).ok());
  assert(nl.gates[and_slot.value].propagation_delay_ps == 0);
  // Write, re-set (last write wins), legal on sequential kinds too.
  assert(logic::set_gate_delay(nl, and_slot.value, 1500).ok());
  assert(nl.gates[and_slot.value].propagation_delay_ps == 1500);
  assert(logic::set_gate_delay(nl, and_slot.value, 2000).ok());
  assert(nl.gates[and_slot.value].propagation_delay_ps == 2000);
  assert(logic::set_gate_delay(nl, dff_slot.value, 42).ok());
  assert(nl.gates[dff_slot.value].propagation_delay_ps == 42);
  // Zero is a legal write equal to clearing.
  assert(logic::set_gate_delay(nl, and_slot.value, 0).ok());
  assert(!logic::clear_gate_delay(nl, and_slot.value).ok());
  // Clear path stores zero.
  assert(logic::set_gate_delay(nl, and_slot.value, 7).ok());
  assert(logic::clear_gate_delay(nl, and_slot.value).ok());
  assert(nl.gates[and_slot.value].propagation_delay_ps == 0);
}

// US1 (T008): verb lifecycle + display pins — the quickstart Scenario 1
// flow with canonical-ps echoes, last-write-wins, zero-is-clean, and
// loud usage refusals that touch no state.
void test_set_delay_verb_lifecycle() {
  wengine::cli::logic::reset_logic_workspace();
  std::string text;
  assert(run_line("logic add AND", text) == 0);
  assert(run_line("logic add NOT", text) == 0);
  assert(run_line("logic wire AND1.Y NOT1.1", text) == 0);
  // Canonical ps echo regardless of the token form used.
  assert(run_line("logic set-delay AND1 1.5ns", text) == 0);
  assert(text == "set AND1 delay=1500ps\n");
  assert(run_line("logic set-delay NOT1 250", text) == 0);
  assert(text == "set NOT1 delay=250ps\n");
  // The annotation is visible in list, on exactly the annotated gates.
  assert(run_line("logic list", text) == 0);
  assert(contains(text, "AND1 AND inputs=2 fanout=1 delay=1500ps"));
  assert(contains(text, "NOT1 NOT inputs=1 fanout=0 delay=250ps"));
  // Re-set replaces (last write wins, FR-011).
  assert(run_line("logic set-delay AND1 2ns", text) == 0);
  assert(text == "set AND1 delay=2000ps\n");
  assert(run_line("logic list", text) == 0);
  assert(contains(text, "delay=2000ps"));
  assert(!contains(text, "delay=1500ps"));
  // Zero is the un-annotated state: the AND1 line goes clean (NOT1 keeps
  // its annotation — the check is per gate, not global).
  assert(run_line("logic set-delay AND1 0", text) == 0);
  assert(text == "set AND1 delay=0ps\n");
  assert(run_line("logic list", text) == 0);
  assert(!contains(text, "AND1 AND inputs=2 fanout=1 delay="));
  assert(contains(text, "NOT1 NOT inputs=1 fanout=0 delay=250ps"));
  // Loud usage refusals, workspace untouched (exit 2 throughout).
  assert(run_line("logic set-delay AND9 5ns", text) == 2); // unknown gate
  assert(run_line("logic set-delay AND1", text) == 2); // missing operand
  assert(run_line("logic set-delay AND1 5ns extra", text) == 2); // stray
  assert(run_line("logic set-delay AND1 -1", text) == 2);
  assert(contains(text, "non-negative"));
  assert(run_line("logic set-delay AND1 1.5ps", text) == 2);
  assert(run_line("logic set-delay AND1 6000h", text) == 2);
  assert(run_line("logic set-delay AND1 1500V", text) == 2);
  // No refusal changed any state: AND1 still clean, NOT1 still 250ps.
  assert(run_line("logic list", text) == 0);
  assert(!contains(text, "AND1 AND inputs=2 fanout=1 delay="));
  assert(contains(text, "NOT1 NOT inputs=1 fanout=0 delay=250ps"));
  wengine::cli::logic::reset_logic_workspace();
}

// US1 (T008): un-annotated workspaces list byte-identically to
// pre-feature (present-only annotation — the 012 pull-display
// precedent). Annotate fully, clear fully, and the bytes return.
void test_unannotated_list_byte_identical() {
  wengine::cli::logic::reset_logic_workspace();
  std::string text;
  assert(run_line("logic add AND", text) == 0);
  assert(run_line("logic add NOT", text) == 0);
  assert(run_line("logic wire AND1.Y NOT1.1", text) == 0);
  assert(run_line("logic list", text) == 0);
  const std::string before = text;
  assert(!contains(before, "delay="));
  // Full annotate-then-clear cycle returns the exact bytes.
  assert(run_line("logic set-delay AND1 1.5ns", text) == 0);
  assert(run_line("logic set-delay NOT1 250ps", text) == 0);
  assert(run_line("logic list", text) == 0);
  assert(text != before);
  assert(run_line("logic set-delay AND1 0", text) == 0);
  assert(run_line("logic set-delay NOT1 0", text) == 0);
  assert(run_line("logic list", text) == 0);
  assert(text == before);
  wengine::cli::logic::reset_logic_workspace();
}

// US2 (T009): the annotation is inert — stored, never interpreted.
//
// No-hot-path proof (re-run of the research.md "Facts that bind"
// reader inventory): the only `propagation_delay_ps` readers in the
// tree are save/check/replay (persistence), the O2 emit reset
// (optimize_o2_emit.hpp:195), display (list), and tests. No
// evaluation, stepping, truth-table, export, or draw path reads the
// field — so the same netlist with every gate heavily annotated must
// produce byte-identical solve/step/truth/export-truth output. The
// export *schema* is the one artifact allowed to differ: it carries the
// stored values (pinned to differ exactly in `delay_ps` rows).
std::string read_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

void test_annotation_inert() {
  // Phase A: un-annotated baseline. Pulled dangling input (AND1.2 up),
  // tied pair (AND1.1 = DFF1.CLK), flop fed by AND1.Y — the annotation
  // later lands on gates feeding pulled/tied nets to prove no topology
  // interplay. `logic reset` (not just the netlist wipe) so the
  // sequential session state restarts at cycle 0 in both phases.
  std::string text;
  assert(run_line("logic reset", text) == 0);
  assert(run_line("logic add AND --inputs 2", text) == 0);
  assert(run_line("logic add DFF", text) == 0);
  assert(run_line("logic wire AND1.Y DFF1.D", text) == 0);
  assert(run_line("logic pull AND1.2 up", text) == 0);
  assert(run_line("logic tie AND1.1 DFF1.CLK", text) == 0);
  assert(run_line("logic solve --in 1", text) == 0);
  const std::string solve1_a = text;
  assert(run_line("logic solve --in 0", text) == 0);
  const std::string solve0_a = text;
  assert(run_line("logic truth", text) == 0);
  const std::string truth_a = text;
  assert(run_line("logic step", text) == 0);
  const std::string step1_a = text;
  assert(run_line("logic step", text) == 0);
  const std::string step2_a = text;
  assert(run_line("logic export --truth /tmp/opencode/delay_inert_a.csv --schema /tmp/opencode/delay_inert_a.yaml", text) == 0);

  // Phase B: identical build, every gate heavily annotated.
  assert(run_line("logic reset", text) == 0);
  assert(run_line("logic add AND --inputs 2", text) == 0);
  assert(run_line("logic add DFF", text) == 0);
  assert(run_line("logic wire AND1.Y DFF1.D", text) == 0);
  assert(run_line("logic pull AND1.2 up", text) == 0);
  assert(run_line("logic tie AND1.1 DFF1.CLK", text) == 0);
  assert(run_line("logic set-delay AND1 1.5h", text) == 0);
  assert(run_line("logic set-delay DFF1 9999999999999999999", text) == 0);
  assert(run_line("logic solve --in 1", text) == 0);
  assert(text == solve1_a);
  assert(run_line("logic solve --in 0", text) == 0);
  assert(text == solve0_a);
  assert(run_line("logic truth", text) == 0);
  assert(text == truth_a);
  assert(run_line("logic step", text) == 0);
  assert(text == step1_a);
  assert(run_line("logic step", text) == 0);
  assert(text == step2_a);
  assert(run_line("logic export --truth /tmp/opencode/delay_inert_b.csv --schema /tmp/opencode/delay_inert_b.yaml", text) == 0);
  // Evaluation artifact identical; storage artifact carries the values.
  assert(read_file("/tmp/opencode/delay_inert_b.csv") == read_file("/tmp/opencode/delay_inert_a.csv"));
  const std::string schema_a = read_file("/tmp/opencode/delay_inert_a.yaml");
  const std::string schema_b = read_file("/tmp/opencode/delay_inert_b.yaml");
  assert(schema_a != schema_b);
  assert(contains(schema_a, "delay_ps: 0"));
  assert(!contains(schema_a, "delay_ps: 1500"));
  assert(contains(schema_b, "delay_ps: 5400000000000000"));
  assert(contains(schema_b, "delay_ps: 9999999999999999999"));
  wengine::cli::logic::reset_logic_workspace();
}

// US3 (T011): clear lifecycle — `unset-delay`, the zero equivalence,
// removal clearing, save/reload round-trip, pre-feature fixture,
// optimizer preserve/reset, world binary checkpoint.
//
// NOTE (TDD): the `unset-delay` halves fail until T012 lands and the
// O2-note half fails until T013 lands; the persistence halves pass on
// the untouched store (that is the point — the schema already carries
// the field).
void test_unset_delay_lifecycle() {
  wengine::cli::logic::reset_logic_workspace();
  std::string text;
  assert(run_line("logic add AND", text) == 0);
  assert(run_line("logic add NOT", text) == 0);
  assert(run_line("logic set-delay AND1 1500", text) == 0);
  // Clear path: success line, listing clean.
  assert(run_line("logic unset-delay AND1", text) == 0);
  assert(text == "cleared AND1 delay\n");
  assert(run_line("logic list", text) == 0);
  assert(!contains(text, "delay="));
  // Lifecycle refusal: nothing to clear (the `unpull` precedent).
  assert(run_line("logic unset-delay AND1", text) == 2);
  assert(contains(text, "no delay set on AND1"));
  assert(run_line("logic unset-delay NOT1", text) == 2);
  assert(contains(text, "no delay set on NOT1"));
  // Unknown gates and strays stay loud, state untouched.
  assert(run_line("logic unset-delay AND9", text) == 2);
  assert(run_line("logic unset-delay", text) == 2);
  assert(run_line("logic unset-delay AND1 extra", text) == 2);
  // set-delay X 0 is equivalent to clearing: indistinguishable in
  // every flow (list here; save/load below).
  assert(run_line("logic set-delay NOT1 250", text) == 0);
  assert(run_line("logic set-delay NOT1 0", text) == 0);
  assert(run_line("logic list", text) == 0);
  assert(!contains(text, "delay="));
  assert(run_line("logic unset-delay NOT1", text) == 2);
  wengine::cli::logic::reset_logic_workspace();
}

// US3 (T011): an annotation dies with its gate; a recycled slot starts
// un-annotated (netlist.hpp:103/:764 — pinned, not built).
void test_remove_recycles_clean() {
  wengine::cli::logic::reset_logic_workspace();
  std::string text;
  assert(run_line("logic add AND", text) == 0);
  assert(run_line("logic set-delay AND1 1500", text) == 0);
  assert(run_line("logic remove AND1", text) == 0);
  assert(run_line("logic add AND", text) == 0); // AND2, recycled slot
  assert(contains(text, "AND2"));
  assert(run_line("logic list", text) == 0);
  assert(!contains(text, "delay="));
  wengine::cli::logic::reset_logic_workspace();
}

// US3 (T011): verb-written round-trip — set via the verb, save carries
// the value, load restores it, replay solves identically. Clear
// round-trip: the second file reads delay_ps: 0.
void test_save_load_round_trip() {
  wengine::cli::logic::reset_logic_workspace();
  std::string text;
  assert(run_line("logic reset", text) == 0);
  assert(run_line("logic add AND", text) == 0);
  assert(run_line("logic add NOT", text) == 0);
  assert(run_line("logic wire AND1.Y NOT1.1", text) == 0);
  assert(run_line("logic set-delay AND1 1.5ns", text) == 0);
  assert(run_line("logic save /tmp/opencode/delay_rt.yaml", text) == 0);
  const std::string saved = read_file("/tmp/opencode/delay_rt.yaml");
  assert(contains(saved, "delay_ps: 1500"));
  assert(run_line("logic solve --in 11", text) == 0);
  const std::string solve_before = text;
  assert(run_line("logic reset", text) == 0);
  assert(run_line("logic load /tmp/opencode/delay_rt.yaml", text) == 0);
  assert(run_line("logic list", text) == 0);
  assert(contains(text, "delay=1500ps"));
  assert(run_line("logic solve --in 11", text) == 0);
  assert(text == solve_before);
  // Clear, save again: the row reads delay_ps: 0 and loads cleanly.
  assert(run_line("logic set-delay AND1 0", text) == 0);
  assert(run_line("logic save /tmp/opencode/delay_rt_cleared.yaml", text) == 0);
  assert(contains(read_file("/tmp/opencode/delay_rt_cleared.yaml"), "delay_ps: 0"));
  assert(run_line("logic reset", text) == 0);
  assert(run_line("logic load /tmp/opencode/delay_rt_cleared.yaml", text) == 0);
  assert(run_line("logic list", text) == 0);
  assert(!contains(text, "delay="));
  wengine::cli::logic::reset_logic_workspace();
}

// US3 (T011): the pre-feature tieoff fixture — already carrying stored
// delays (delay_ps: 1500/250) — loads and solves exactly as before.
void test_tieoff_prefeature_loads() {
  wengine::cli::logic::reset_logic_workspace();
  std::string text;
  assert(run_line(std::string("logic load ") + WENGINE_LOGIC_EXAMPLE_DIR "/tieoff.yaml", text) == 0);
  assert(run_line("logic list", text) == 0);
  assert(contains(text, "delay=1500ps"));
  assert(contains(text, "delay=250ps"));
  assert(run_line("logic solve --from AND1.1=1", text) == 0);
  assert(contains(text, "NOT1.Y=1"));
  wengine::cli::logic::reset_logic_workspace();
}

// US3 (T011): optimizer policy — O0/O1 preserve in place with no note;
// O2 resets per its documented contract and reports the reset iff >=1
// annotation existed; optimize-of-optimize is idempotent.
//
// The netlist is two unwired gates: O1 provably keeps both (nothing to
// fuse/fold), so the preserve pin is structural, not coincidental. (A
// wired AND->NOT pair fuses into NAND1 at O1 — the annotation then dies
// with its gate, the same rule as `remove`.)
void test_optimizer_delay_policy() {
  wengine::cli::logic::reset_logic_workspace();
  std::string text;
  assert(run_line("logic add AND", text) == 0);
  assert(run_line("logic add NOT", text) == 0);
  assert(run_line("logic set-delay AND1 1500", text) == 0);
  assert(run_line("logic set-delay NOT1 250", text) == 0);
  assert(run_line("logic optimize --level O0", text) == 0);
  assert(!contains(text, "propagation delay"));
  assert(run_line("logic list", text) == 0);
  assert(contains(text, "delay=1500ps"));
  assert(contains(text, "delay=250ps"));
  assert(run_line("logic optimize --level O1", text) == 0);
  assert(!contains(text, "propagation delay"));
  assert(run_line("logic list", text) == 0);
  assert(contains(text, "delay=1500ps"));
  assert(contains(text, "delay=250ps"));
  // O2 resets with the conditional report note (T013: fails until then).
  assert(run_line("logic optimize --level O2", text) == 0);
  assert(contains(text, "note: O2 reshaped the network; reset the propagation delay on 2 gate(s) (docs/logic.md §12)"));
  assert(run_line("logic list", text) == 0);
  assert(!contains(text, "delay="));
  // Second O2: nothing to reset, no note (idempotent).
  assert(run_line("logic optimize --level O2", text) == 0);
  assert(!contains(text, "propagation delay"));
  // Zero-annotation O2 prints nothing extra (byte-identical).
  assert(run_line("logic reset", text) == 0);
  assert(run_line("logic add AND", text) == 0);
  assert(run_line("logic add NOT", text) == 0);
  assert(run_line("logic optimize --level O2", text) == 0);
  assert(!contains(text, "propagation delay"));
  wengine::cli::logic::reset_logic_workspace();
}

// US3 (T011): world binary checkpoint round-trips the u64 column
// (serialize.hpp:352/:791) with verb-written values. The seq pool is
// populated the way the solver lowering does (init_seq_state plus
// interface-sized stimulus/outputs) — the checkpoint's stimulus
// length check demands it, same as any solver-owned state.
void test_world_checkpoint_round_trip() {
  world::State src;
  const auto a = logic::add_gate(src.netlist, logic::GateKind::kAnd, 2);
  assert(a.ok());
  const auto n = logic::add_gate(src.netlist, logic::GateKind::kNot, 1);
  assert(n.ok());
  assert(logic::wire(src.netlist, a.value, n.value, 0).ok());
  assert(logic::set_gate_delay(src.netlist, a.value, 1500).ok());
  assert(logic::set_gate_delay(src.netlist, n.value, 250).ok());
  logic::init_seq_state(src.seq, src.netlist);
  src.seq_inputs.assign(logic::block_input_count(src.netlist), logic::LogicValue::kLow);
  src.seq_members.clear();
  src.seq_outputs.assign(logic::block_output_count(src.netlist), logic::LogicValue::kLow);
  std::ostringstream bytes;
  assert(world::save_state(src, bytes).ok());
  world::State dst;
  std::istringstream in(bytes.str());
  assert(world::load_state(dst, in).ok());
  assert(dst == src);
  assert(dst.netlist.gates[a.value].propagation_delay_ps == 1500);
  assert(dst.netlist.gates[n.value].propagation_delay_ps == 250);
}

// Run a whole session script on both front ends from identical states;
// every line's code and bytes must match (TUI parity by construction).
void check_tui_script(const std::vector<std::string> &lines) {
  std::vector<std::pair<int, std::string>> via_exec;
  {
    wengine::cli::logic::reset_logic_workspace();
    wengine::tui::TuiExecutor exec;
    for (const std::string &line : lines) {
      const wengine::tui::ExecResult r = exec.execute_line(line);
      via_exec.emplace_back(r.rc, r.text);
    }
  }
  std::vector<std::pair<int, std::string>> via_shell;
  {
    wengine::cli::logic::reset_logic_workspace();
    wengine::cli::Shell shell;
    shell.set_history_file("");
    for (const std::string &line : lines) {
      std::ostringstream out;
      const int rc = shell.execute(line, out);
      via_shell.emplace_back(rc, out.str());
    }
  }
  assert(via_exec.size() == via_shell.size());
  for (std::size_t i = 0; i < via_exec.size(); ++i) {
    assert(via_exec[i].first == via_shell[i].first);
    assert(via_exec[i].second == via_shell[i].second);
  }
  wengine::cli::logic::reset_logic_workspace();
}

// US4 (T015): the new vocabulary is byte-identical across the TUI
// executor and the Shell (both ride cli::Shell::execute verbatim — no
// TUI source change, no logic form exists).
void test_tui_parity() {
  check_tui_script({
      "logic reset",
      "logic add AND",
      "logic add NOT",
      "logic wire AND1.Y NOT1.1",
      "logic set-delay AND1 1.5ns",
      "logic set-delay NOT1 250",
      "logic list",
      "logic solve --in 11",
      "logic set-delay AND1 2ns",
      "logic unset-delay NOT1",
      "logic list",
      "logic set-delay AND1 -1",
      "logic unset-delay NOT1",
      "logic set-delay AND9 5ns",
  });
}

} // namespace

int main() {
  test_parse_time_ps_goldens();
  test_gate_delay_seam();
  test_set_delay_verb_lifecycle();
  test_unannotated_list_byte_identical();
  test_annotation_inert();
  test_unset_delay_lifecycle();
  test_remove_recycles_clean();
  test_save_load_round_trip();
  test_tieoff_prefeature_loads();
  test_optimizer_delay_policy();
  test_world_checkpoint_round_trip();
  test_tui_parity();
  std::cout << "test_delay: OK\n";
  return 0;
}
