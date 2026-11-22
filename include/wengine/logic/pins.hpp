#pragma once

/**
 * @file
 * Pin + kind vocabulary module (C2 deepening) of the logic pipeline: the one table per
 * gate kind that everything parses against. Each row owns the display
 * name, default fan-in, exact allowed fan-in set, and named input pins
 * with per-pin minimum fan-in, so adding a kind is one row, not edits
 * across functions. This header is the parse/authoring stage of the
 * pipeline (netlist parse -> state -> eval -> layout/topo -> optimize):
 * it turns gate/pin text into slots and PinRefs that netlist.hpp
 * mutates, topo.hpp orders, eval.hpp solves, and layout.hpp draws.
 *
 * Dependency rules (docs/logic.md section 4): header-only,
 * allocation-free, exception-free; depends only on types.hpp plus
 * core/status (parsing returns core::Result, never throws). Logic is a
 * standalone domain: this header must never depend on CLI types.
 * Consumers: netlist.hpp, yaml_store.hpp, the CLI `logic` verbs, and
 * the test_logic* suite.
 */

#include "wengine/core/status.hpp"
#include "wengine/logic/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace wengine::logic {

/**
 * @brief Ordinal of a gate kind: its byte value (0..7 combinational,
 *        16..20 sequential), used to index Netlist::next_counter.
 *
 * @param[in] k Kind to convert.
 * @return Underlying byte value of k.
 */
[[nodiscard]] constexpr std::uint8_t kind_ordinal(GateKind k) noexcept {
  return static_cast<std::uint8_t>(k);
}

namespace detail {
/**
 * @brief Uppercase one ASCII letter; other characters pass through.
 *
 * @param[in] c Character to convert.
 * @return Uppercase form of c for 'a'..'z', else c unchanged.
 */
[[nodiscard]] constexpr char to_upper(char c) noexcept {
  if (c >= 'a' && c <= 'z') {
    return static_cast<char>(c - ('a' - 'A'));
  }
  return c;
}

/**
 * @brief Case-insensitive prefix test over uppercase ASCII.
 *
 * @param[in] text Text to test.
 * @param[in] prefix Uppercase prefix to look for.
 * @return True when text starts with prefix ignoring case.
 */
[[nodiscard]] constexpr bool starts_with_ci(std::string_view text, std::string_view prefix) noexcept {
  if (text.size() < prefix.size()) {
    return false;
  }
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (to_upper(text[i]) != prefix[i]) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Strict unsigned decimal parse: no signs, no spaces.
 *
 * @param[in] text Digits to parse; must be non-empty.
 * @param[in] limit Upper bound; values that would exceed it fail.
 * @param[out] out Parsed value on success.
 * @return True on success; false on empty text, non-digits, or
 *         overflow past limit.
 */
[[nodiscard]] constexpr bool parse_decimal(std::string_view text, std::uint32_t limit, std::uint32_t &out) noexcept {
  if (text.empty()) {
    return false;
  }
  std::uint32_t v = 0;
  for (char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    const auto d = static_cast<std::uint32_t>(c - '0');
    if (v > (limit - d) / 10) {
      return false;
    }
    v = v * 10 + d;
  }
  out = v;
  return true;
}
/**
 * @brief Parsed gate identity: kind ordinal + 1-based counter.
 *
 * Single accept-set owner for every gate-name lookup (LOGIC bulk-load
 * split): both the linear scan and the indexed bulk adapter parse through
 * parse_gate_id below, so their accept sets agree by construction, not
 * by comment.
 */
struct ParsedGateId {
  /** @brief Kind ordinal (kind_ordinal value, 0..8 + 16..20). */
  std::uint8_t ordinal = 0;
  /** @brief 1-based per-kind counter (>= 1). */
  std::uint32_t counter = 0;
};
} // namespace detail

/**
 * @brief Exact case-insensitive pin-name match ("clk" == "CLK").
 *
 * @param[in] suffix Pin suffix text after the dot.
 * @param[in] name Uppercase table spelling to compare against.
 * @return True on exact length + case-insensitive match; empty names
 *         never match. Non-matching (longer/shorter) suffixes fall
 *         through to the numeric parser at the call site.
 */
[[nodiscard]] constexpr bool match_pin_name(std::string_view suffix, std::string_view name) noexcept {
  if (name.empty() || suffix.size() != name.size()) {
    return false;
  }
  for (std::size_t i = 0; i < suffix.size(); ++i) {
    if (detail::to_upper(suffix[i]) != name[i]) {
      return false;
    }
  }
  return true;
}

/**
 * @brief One named input pin of a sequential kind: 0-based input plus
 *        the minimum fan-in that exposes it, the schematic anchor row,
 *        and the clock-wedge / latch-notch marks.
 *
 * DFF RST/SET/EN exist only on wider flops (bare 2-input flops reject
 * them like out-of-range numerics). anchor_row is the role row
 * (1..5 of 6ths; -1 for combinational kinds without role anchors);
 * is_clock marks the edge-flop wedge pin (row 2), is_notch the
 * DLATCH enable notch (row 2).
 */
struct KindPin {
  /** @brief Uppercase pin spelling (e.g. "CLK", "RST"). */
  std::string_view name;
  /** @brief 0-based input index this name resolves to. */
  std::uint8_t input = 0;
  /** @brief Minimum gate fan-in exposing this pin. */
  std::uint8_t min_inputs = 0;
  /** @brief Schematic role row (1..5 of 6ths); -1 when the kind has none. */
  std::int8_t anchor_row = -1;
  /** @brief True for the edge-flop clock-wedge pin. */
  bool is_clock = false;
  /** @brief True for the level-latch enable-notch pin. */
  bool is_notch = false;
};

/**
 * @brief One gate kind: parse name, default fan-in, exact allowed
 *        fan-in set, named input pins, and sequential face letters.
 *
 * allowed_mask holds bit (n-1) iff fan-in n (1..8) is valid; pins are
 * the named sequential inputs (empty for combinational kinds); face
 * holds the schematic face letters for sequential kinds (empty for
 * combinational).
 */
struct KindInfo {
  /** @brief Gate kind this row describes. */
  GateKind kind;
  /** @brief Uppercase parse/display spelling (e.g. "DFF"). */
  std::string_view name;
  /** @brief Fan-in used when creation omits the count. */
  std::uint8_t default_inputs = 2;
  /** @brief Bit (n-1) set iff fan-in n (1..8) is valid. */
  std::uint16_t allowed_mask = 0;
  /** @brief Number of valid entries in pins. */
  std::uint8_t pin_count = 0;
  /** @brief Named sequential input pins. */
  KindPin pins[5]{};
  /** @brief Schematic face letters; empty for combinational kinds. */
  std::string_view face{};
};

/**
 * @brief The kind table: every known kind in one append-only place.
 *
 * @note Append-only: persisted byte values (DFF=16, DLATCH=17, JK=18,
 *       T=19, SRLATCH=20) must never shift. Fan-in masks: combinational
 *       2..8 = 0xFE; NOT/BUF exactly 1 = 0x01; TBUF exactly 2 = 0x02;
 *       DFF {2,5} = 0x12; DLATCH/T/SRLATCH exactly 2 = 0x02; JK exactly
 *       3 = 0x04.
 */
inline constexpr KindInfo kKinds[] = {
    {GateKind::kAnd, "AND", 2, 0xFEu, 0, {}},
    {GateKind::kOr, "OR", 2, 0xFEu, 0, {}},
    {GateKind::kNand, "NAND", 2, 0xFEu, 0, {}},
    {GateKind::kNor, "NOR", 2, 0xFEu, 0, {}},
    {GateKind::kXor, "XOR", 2, 0xFEu, 0, {}},
    {GateKind::kXnor, "XNOR", 2, 0xFEu, 0, {}},
    {GateKind::kNot, "NOT", 1, 0x01u, 0, {}},
    {GateKind::kBuf, "BUF", 1, 0x01u, 0, {}},
    {GateKind::kTbuf, "TBUF", 2, 0x02u, 0, {}},
    {GateKind::kDff, "DFF", 2, 0x12u, 5,
     {{"D", 0, 2, 1}, {"CLK", 1, 2, 2, true}, {"RST", 2, 3, 3}, {"SET", 3, 4, 4}, {"EN", 4, 5, 5}}, "D"},
    {GateKind::kDlatch, "DLATCH", 2, 0x02u, 2, {{"D", 0, 2, 1}, {"EN", 1, 2, 2, false, true}}, "DL"},
    {GateKind::kJk, "JK", 3, 0x04u, 3, {{"J", 0, 3, 1}, {"K", 1, 3, 4}, {"CLK", 2, 3, 2, true}}, "JK"},
    {GateKind::kT, "T", 2, 0x02u, 2, {{"T", 0, 2, 1}, {"CLK", 1, 2, 2, true}}, "T"},
    {GateKind::kSrlatch, "SRLATCH", 2, 0x02u, 2, {{"S", 0, 2, 1}, {"R", 1, 2, 4}}, "SR"},
};

/**
 * @brief Compile-time table self-check: names, masks, defaults, pin
 *        rows, anchors, and clock/notch/face marks all agree.
 *
 * @return True when the table is internally consistent.
 */
[[nodiscard]] constexpr bool kind_table_valid() noexcept {
  std::size_t n = 0;
  for (const KindInfo &e : kKinds) {
    (void)e;
    ++n;
  }
  if (n != 14) {
    return false;
  }
  for (const KindInfo &e : kKinds) {
    if (e.name.empty() || e.allowed_mask == 0) {
      return false;
    }
    if (e.default_inputs == 0 || e.default_inputs > kMaxInputs) {
      return false;
    }
    if ((e.allowed_mask & static_cast<std::uint16_t>(1u << (e.default_inputs - 1u))) == 0) {
      return false; // default fan-in must itself be allowed
    }
    if (e.pin_count > 5) {
      return false;
    }
    for (std::uint8_t p = 0; p < e.pin_count; ++p) {
      if (e.pins[p].name.empty() || e.pins[p].input >= kMaxInputs) {
        return false;
      }
      if (e.pins[p].min_inputs <= e.pins[p].input) {
        return false; // min fan-in always exceeds the 0-based index
      }
      if (is_sequential_kind(e.kind)) {
        if (e.pins[p].anchor_row < 1 || e.pins[p].anchor_row > 5) {
          return false; // role row 1..5 of 6ths (contract C2)
        }
        if ((e.pins[p].is_clock && e.pins[p].anchor_row != 2) ||
            (e.pins[p].is_notch && e.pins[p].anchor_row != 2)) {
          return false; // clock wedge + latch notch both sit at row 2 (I-CLK)
        }
        for (std::uint8_t q = 0; q < p; ++q) {
          if (e.pins[q].anchor_row == e.pins[p].anchor_row) {
            return false; // rows unique per kind
          }
        }
      } else if (e.pins[p].anchor_row != -1 || e.pins[p].is_clock || e.pins[p].is_notch) {
        return false; // combinational pins carry no role anchors
      }
    }
    if (is_sequential_kind(e.kind)) {
      if (e.face.empty()) {
        return false; // flops/latches need face letters (contract C5)
      }
      int clocks = 0;
      int notches = 0;
      for (std::uint8_t p = 0; p < e.pin_count; ++p) {
        if (e.pins[p].is_clock) {
          ++clocks;
        }
        if (e.pins[p].is_notch) {
          ++notches;
        }
      }
      const bool want_clock = (e.kind == GateKind::kDff || e.kind == GateKind::kJk || e.kind == GateKind::kT);
      const bool want_notch = (e.kind == GateKind::kDlatch);
      if ((want_clock ? clocks != 1 : clocks != 0) || (want_notch ? notches != 1 : notches != 0)) {
        return false;
      }
    } else if (!e.face.empty()) {
      return false;
    }
  }
  return true;
}

static_assert(kind_table_valid(), "logic kind table: default/mask/pin rows inconsistent");

namespace detail {
/**
 * @brief Parse `<KIND><counter>` (case-insensitive kind, 1-based decimal).
 *
 * Iterates kKinds in table order, first valid (prefix + counter >= 1)
 * wins — the exact loop the old resolve_gate / resolve_gate_indexed
 * duplicates shared by comment. Empty text fails (caller maps to
 * "bad gate name: empty"); no valid kind/counter fails ("bad gate
 * name" at the call site).
 *
 * @param[in] name Gate display text (e.g. "AND12").
 * @param[out] out Ordinal + counter on success.
 * @return True on a well-formed kind + counter, false otherwise.
 */
[[nodiscard]] constexpr bool parse_gate_id(std::string_view name, ParsedGateId &out) noexcept {
  if (name.empty()) {
    return false;
  }
  for (const KindInfo &e : kKinds) {
    const std::string_view kind_name = e.name;
    if (!starts_with_ci(name, kind_name)) {
      continue;
    }
    const std::string_view num = name.substr(kind_name.size());
    std::uint32_t counter = 0;
    if (!parse_decimal(num, 0xFFFFFFFFu, counter) || counter == 0) {
      continue; // not this kind (or bad number): keep trying other kinds
    }
    out.ordinal = kind_ordinal(e.kind);
    out.counter = counter;
    return true;
  }
  return false;
}
} // namespace detail

/**
 * @brief True iff fan-in n is allowed for kind k (table mask lookup).
 *
 * @param[in] k Gate kind to check.
 * @param[in] n Fan-in to check (0 or above kMaxInputs is always false).
 * @return True when the kind row allows n.
 */
[[nodiscard]] constexpr bool valid_fanin(GateKind k, std::uint8_t n) noexcept {
  if (n == 0 || n > kMaxInputs) {
    return false;
  }
  for (const KindInfo &e : kKinds) {
    if (e.kind == k) {
      return (e.allowed_mask & static_cast<std::uint16_t>(1u << (n - 1u))) != 0;
    }
  }
  return false;
}

/**
 * @brief Default fan-in for kind k (table lookup; 2 for unknown kinds).
 *
 * @param[in] k Gate kind to look up.
 * @return Default input count for k.
 */
[[nodiscard]] constexpr std::uint8_t default_fanin(GateKind k) noexcept {
  for (const KindInfo &e : kKinds) {
    if (e.kind == k) {
      return e.default_inputs;
    }
  }
  return 2;
}

/**
 * @brief Parse a gate kind name, case-insensitively, exact full match.
 *
 * Sequential kinds (DFF/DLATCH/JK/T/SRLATCH) parse here; creation
 * validates fan-in via valid_fanin and state attaches via
 * init_seq_state.
 *
 * @param[in] text Kind spelling (e.g. "and", "DFF").
 * @return kOk with the kind on success; kInvalidArgument for empty
 *         text or an unknown kind spelling.
 */
[[nodiscard]] inline core::Result<GateKind> parse_gate_kind(std::string_view text) noexcept {
  if (text.empty()) {
    return core::Result<GateKind>::failure("bad gate kind: empty");
  }
  for (const KindInfo &e : kKinds) {
    if (text.size() != e.name.size()) {
      continue;
    }
    bool same = true;
    for (std::size_t j = 0; j < text.size(); ++j) {
      if (detail::to_upper(text[j]) != e.name[j]) {
        same = false;
        break;
      }
    }
    if (same) {
      return core::Result<GateKind>::success(e.kind);
    }
  }
  return core::Result<GateKind>::failure("bad gate kind: want AND|OR|NAND|NOR|XOR|XNOR|NOT|BUF|TBUF|DFF|DLATCH|JK|T|SRLATCH");
}

/**
 * @brief Find a live gate by display name (e.g. "AND1", "DFF1").
 *
 * Linear slot-order scan: deterministic, first (only) match wins;
 * covers all sequential kinds (ordinals compare by value, so no
 * Netlist shape change was needed).
 *
 * @param[in] nl Netlist to search.
 * @param[in] name Case-insensitive kind prefix plus 1-based counter.
 * @return kOk with the slot on success; kInvalidArgument for empty
 *         text, a bad kind prefix/number ("bad gate name"), or a
 *         well-formed but absent gate ("unknown gate").
 */
[[nodiscard]] inline core::Result<Slot> resolve_gate(const Netlist &nl, std::string_view name) noexcept {
  if (name.empty()) {
    return core::Result<Slot>::failure("bad gate name: empty");
  }
  detail::ParsedGateId id{};
  if (!detail::parse_gate_id(name, id)) {
    return core::Result<Slot>::failure("bad gate name");
  }
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const Gate &g = nl.slots[s];
    if (g.live && kind_ordinal(g.kind) == id.ordinal && g.counter == id.counter) {
      return core::Result<Slot>::success(s);
    }
  }
  return core::Result<Slot>::failure("unknown gate");
}

// --- PinResolution module (bulk-load split) ---------------------------------
//
// Two adapters over the one parse_gate_id accept set: LinearScan for
// single-pin interactive verbs (O(N) scan, trivial cost) and IndexedBulk
// for bulk replay (O(1) per pin after one O(N) build). Both expose
// `resolve(name)` + `operator()` with the exact resolve_gate error
// vocabulary, so parse_pin_with below takes either without duplication.
// resolve() stays allocation-free/noexcept on both; only IndexedBulk::build
// (cold bulk path) allocates.

/**
 * @brief Single-pin gate lookup over the live slot scan.
 *
 * Thin adapter around the slot-order scan (deterministic, first match
 * wins). Interactive verbs use this directly; bulk replay prefers
 * IndexedBulk.
 */
struct LinearScanResolver {
  /** @brief Netlist searched (non-owning; must outlive use). */
  const Netlist *nl = nullptr;

  /**
   * @brief Bind to the netlist searched.
   * @param[in] n Netlist holding the gates.
   */
  explicit LinearScanResolver(const Netlist &n) noexcept : nl(&n) {}

  /**
   * @brief Resolve a gate display name to its slot.
   * @param[in] name Case-insensitive kind prefix plus 1-based counter.
   * @return Slot on success; "bad gate name: empty" / "bad gate name" /
   *         "unknown gate" otherwise (resolve_gate vocabulary).
   */
  [[nodiscard]] core::Result<Slot> resolve(std::string_view name) const noexcept {
    if (nl == nullptr) {
      return core::Result<Slot>::failure("unknown gate");
    }
    return resolve_gate(*nl, name);
  }

  /**
   * @brief Callable form for parse_pin_with (same as resolve).
   * @param[in] name Gate display text.
   * @return Slot on success; lookup error otherwise.
   */
  [[nodiscard]] core::Result<Slot> operator()(std::string_view name) const noexcept {
    return resolve(name);
  }
};

/**
 * @brief Bulk gate lookup over dense per-kind counter->slot vectors.
 *
 * Counters are per-kind monotonic and dense in bulk loads (never reused,
 * file order == slot order), so a vector indexed by counter replaces the
 * old (ordinal, counter) hash: O(1) lookup, one O(N) build, no hashing.
 * Gaps (sparse counters) read kNoDriver -> "unknown gate", never a
 * mis-resolve.
 */
struct IndexedBulkResolver {
  /** @brief Per-ordinal counter->slot; index == counter, [0] unused (kNoDriver). */
  std::array<std::vector<Slot>, 21> slot_by_counter{};

  /**
   * @brief Drop every table entry (fresh state).
   */
  void clear() {
    for (auto &t : slot_by_counter) {
      t.clear();
    }
  }

  /**
   * @brief Build the index over one netlist (cold bulk path; allocates).
   *
   * Sizes each table to next_counter[ord] (max observed counter + 1),
   * fills kNoDriver, then records every live slot. Duplicate
   * (ordinal, counter) pairs must be refused before building (the
   * loader's duplicate gate-id check); last write would win otherwise.
   *
   * @param[in] nl Netlist whose live slots index.
   */
  void build(const Netlist &nl) {
    clear();
    for (std::size_t o = 0; o < slot_by_counter.size(); ++o) {
      const std::uint32_t need = (o < nl.next_counter.size()) ? nl.next_counter[o] : 1u;
      slot_by_counter[o].assign(static_cast<std::size_t>(need), kNoDriver);
    }
    for (Slot s = 0; s < slot_count(nl); ++s) {
      const Gate &g = nl.slots[s];
      if (!g.live) {
        continue;
      }
      const std::size_t o = static_cast<std::size_t>(kind_ordinal(g.kind));
      if (o >= slot_by_counter.size()) {
        continue; // defensive: unknown ordinal never indexes
      }
      if (static_cast<std::size_t>(g.counter) >= slot_by_counter[o].size()) {
        slot_by_counter[o].resize(static_cast<std::size_t>(g.counter) + 1u, kNoDriver);
      }
      slot_by_counter[o][g.counter] = s;
    }
  }

  /**
   * @brief Resolve a gate display name to its slot (allocation-free).
   * @param[in] name Case-insensitive kind prefix plus 1-based counter.
   * @return Slot on success; "bad gate name: empty" / "bad gate name" /
   *         "unknown gate" otherwise (resolve_gate vocabulary).
   */
  [[nodiscard]] core::Result<Slot> resolve(std::string_view name) const noexcept {
    if (name.empty()) {
      return core::Result<Slot>::failure("bad gate name: empty");
    }
    detail::ParsedGateId id{};
    if (!detail::parse_gate_id(name, id)) {
      return core::Result<Slot>::failure("bad gate name");
    }
    if (static_cast<std::size_t>(id.ordinal) >= slot_by_counter.size()) {
      return core::Result<Slot>::failure("unknown gate");
    }
    const std::vector<Slot> &t = slot_by_counter[static_cast<std::size_t>(id.ordinal)];
    if (static_cast<std::size_t>(id.counter) >= t.size()) {
      return core::Result<Slot>::failure("unknown gate");
    }
    const Slot s = t[static_cast<std::size_t>(id.counter)];
    if (s == kNoDriver) {
      return core::Result<Slot>::failure("unknown gate");
    }
    return core::Result<Slot>::success(s);
  }

  /**
   * @brief Callable form for parse_pin_with (same as resolve).
   * @param[in] name Gate display text.
   * @return Slot on success; lookup error otherwise.
   */
  [[nodiscard]] core::Result<Slot> operator()(std::string_view name) const noexcept {
    return resolve(name);
  }
};

/**
 * @brief Parse `<NAME>.Y` (output) or `<NAME>.<1..k>` (1-based input).
 *
 * Bulk-load seam: the gate-name lookup is a PinResolution adapter
 * (LinearScanResolver for interactive verbs, IndexedBulkResolver for bulk
 * replay) with the exact `resolve_gate` error vocabulary ("unknown gate"
 * for well-formed-but-absent names, anything else for malformed names).
 * Suffix parsing (named sequential pins, Q/NQ, Y, numerics) stays here —
 * the one table-owned spelling, never duplicated at call sites.
 *
 * @param[in] nl Netlist holding the named gate.
 * @param[in] text Pin spelling to parse.
 * @param[in] lookup_gate Gate-name lookup (resolve_gate vocabulary).
 * @return kOk with the PinRef on success; kInvalidArgument for a
 *         malformed pin ("bad pin: ..."), an absent gate
 *         ("unknown gate"), or an out-of-range input index
 *         ("input index out of range").
 */
template <typename GateLookup>
[[nodiscard]] inline core::Result<PinRef> parse_pin_with(const Netlist &nl, std::string_view text, GateLookup &&lookup_gate) {
  std::size_t dot = text.find('.');
  if (dot == std::string_view::npos) {
    return core::Result<PinRef>::failure("bad pin: want NAME.Y|NAME.i");
  }
  if (text.find('.', dot + 1) != std::string_view::npos) {
    return core::Result<PinRef>::failure("bad pin: want NAME.Y|NAME.i");
  }
  const std::string_view gate_text = text.substr(0, dot);
  const std::string_view suffix = text.substr(dot + 1);
  if (gate_text.empty() || suffix.empty()) {
    return core::Result<PinRef>::failure("bad pin: want NAME.Y|NAME.i");
  }
  auto gate = lookup_gate(gate_text);
  if (!gate.ok()) {
    if (gate.error == std::string_view("unknown gate")) {
      return core::Result<PinRef>::failure("unknown gate");
    }
    return core::Result<PinRef>::failure("bad pin: bad gate name");
  }
  const Slot slot = gate.value;
  const Gate &g = nl.slots[slot];
  // Named sequential pins (FR-010): checked before the generic Y/numeric
  // forms; numeric aliases fall through below and keep working.
  if (is_sequential_kind(g.kind)) {
    for (const KindInfo &e : kKinds) {
      if (e.kind != g.kind) {
        continue;
      }
      for (std::uint8_t p = 0; p < e.pin_count; ++p) {
        if (g.input_count >= e.pins[p].min_inputs && match_pin_name(suffix, e.pins[p].name)) {
          PinRef ref{};
          ref.slot = slot;
          ref.is_output = false;
          ref.input = e.pins[p].input;
          return core::Result<PinRef>::success(ref);
        }
      }
      break;
    }
    if (match_pin_name(suffix, "Q")) {
      PinRef ref{};
      ref.slot = slot;
      ref.is_output = true;
      return core::Result<PinRef>::success(ref);
    }
    if (match_pin_name(suffix, "NQ")) {
      PinRef ref{};
      ref.slot = slot;
      ref.is_output = true;
      ref.negated = true;
      return core::Result<PinRef>::success(ref);
    }
  }
  if (suffix.size() == 1 && (suffix[0] == 'Y' || suffix[0] == 'y')) {
    PinRef ref{};
    ref.slot = slot;
    ref.is_output = true;
    return core::Result<PinRef>::success(ref);
  }
  std::uint32_t idx = 0;
  if (!detail::parse_decimal(suffix, 0xFFFFu, idx) || idx == 0) {
    return core::Result<PinRef>::failure("bad pin: want NAME.Y|NAME.i");
  }
  if (idx > g.input_count) {
    return core::Result<PinRef>::failure("input index out of range");
  }
  PinRef ref{};
  ref.slot = slot;
  ref.is_output = false;
  ref.input = static_cast<std::uint8_t>(idx - 1);
  return core::Result<PinRef>::success(ref);
}

/**
 * @brief Parse `<NAME>.Y` (output) or `<NAME>.<1..k>` (1-based input).
 *
 * Sequential gates additionally accept named pins (case-insensitive)
 * from the kind table (DFF `D`/`CLK` plus `RST`/`SET`/`EN` on
 * full-control flops only; DLATCH `D`/`EN`; JK `J`/`K`/`CLK`;
 * T `T`/`CLK`; SRLATCH `S`/`R`; every flop also `Q`/`NQ`). Numeric
 * aliases keep working for all kinds; names on combinational gates are
 * rejected; kind-foreign names (e.g. `D` on a JK flop) fall through to
 * the numeric parser and fail there. Input range validates against the
 * gate's configured fan-in.
 *
 * Thin wrapper over parse_pin_with with the linear resolve_gate
 * lookup (single-pin interactive cost is trivial).
 *
 * @param[in] nl Netlist holding the named gate.
 * @param[in] text Pin spelling to parse.
 * @return kOk with the PinRef on success; kInvalidArgument for a
 *         malformed pin ("bad pin: ..."), an absent gate
 *         ("unknown gate"), or an out-of-range input index
 *         ("input index out of range").
 */
[[nodiscard]] inline core::Result<PinRef> parse_pin(const Netlist &nl, std::string_view text) noexcept {
  return parse_pin_with(nl, text, LinearScanResolver(nl));
}

} // namespace wengine::logic
