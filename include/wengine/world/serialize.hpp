#pragma once

// Versioned binary persistence for world::State (deterministic, no deps).
//
// Format v6, all integers little-endian, doubles as IEEE-754 bits:
//   'W' 'E' 'N' 'G', u32 version (= 6)
//   f64 sim_time, u64 step_index
//   u32 entity_capacity, generations[u32]..., u32 freelist_size, freelist[u32]...
//   u32 particle_capacity, per slot: u8 present + f64 x4 (fixed stride)
//   u32 signal_capacity, per slot: u8 present, u8 kind, u8 quantity,
//     u8 running, u8 op_count, f64 freq/amp/offset/phase/duty/t0/value,
//     u64 seed, u64 rng, u64 serial, u64 ops[kMaxOps],
//     u8 expr_len, u8 expr_op[kMaxExprNodes], f64 expr_arg[kMaxExprNodes],
//     u64 table_off, u64 table_len, u64 lua_off, u64 lua_len
//   u32 table_store_size, f64 table_times[]..., f64 table_values[]...
//   u32 lua_blob_size, u8 lua_text[]... (opaque script bytes)
//   Sequential pool (v6 only, fixed stride over kMaxGates slots):
//     per slot: u8 live, u8 kind, u32 counter, u8 input_count,
//       u8 ignored_mask, u8 ignore_output, u8 ignore_nq_output,
//       u16 drivers[8], u8 driver_inv[8], u16 tie_rep[8],
//       u64 propagation_delay_ps
//     u32 next_counter[21], u16 netlist_live_count
//     per slot: u8 seq_valid, u8 seq_q, u64 seq_serial
//     u64 seq_next_serial, u16 seq_live_count
//     u32 stimulus_len, u8 stimulus[]... (block-interface order, 0/1)
//     u32 members_len, u16 members[]... (edge-flop slots)
//     u8 step_all (1 = all domains, members empty)
//     u32 outputs_len, u8 outputs[]... (settled block outputs, 0/1)
//   Derived data (fanouts, topo order/levels, dirty) is recomputed on
//   load, never stored. v5/v4/v3/v2/v1 files still load: the sequential
//   pool comes back empty (same pattern as v1 signals).
// Dead signal slots (present=0) retain their last live columns verbatim
// (detach clears presence only); load re-checks their sizes/ranges but no
// live-source invariant, so supported despawn/save/load round-trips exactly.
// v4 files (no Lua columns or blob) still load: Lua ranges zero out, the
// script store stays empty. v3 files (no expression/table columns or
// store) still load: new columns zero out, both stores stay empty. v2
// files (no serial/operands) still load: serials synthesize as slot
// indices, operand lists come back empty. v1 files (no signal section)
// still load: signals come back empty.
// Freelist order is stored, so a round-trip preserves exact spawn-reuse
// behavior. load_state() replaces the target; corrupt input (bad magic,
// version, truncation, out-of-range indices) yields kInvalidArgument and
// leaves the target cleared. Write failures yield kInternal.

#include "wengine/core/status.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/seq_state.hpp"
#include "wengine/logic/types.hpp"
#include "wengine/signal/types.hpp"
#include "wengine/world/state.hpp"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <ostream>
#include <utility>
#include <vector>

namespace wengine::world {

inline constexpr std::uint32_t kStateFormatVersion = 6;
// Sanity cap per count field: rejects corrupt files before huge allocs.
inline constexpr std::uint32_t kMaxStateEntries = 1u << 24;

namespace detail {

inline void put_u8(std::ostream &out, std::uint8_t v) {
  out.put(static_cast<char>(v));
}

inline void put_u16_le(std::ostream &out, std::uint16_t v) {
  out.put(static_cast<char>(v & 0xFFu));
  out.put(static_cast<char>((v >> 8) & 0xFFu));
}

inline void put_u32_le(std::ostream &out, std::uint32_t v) {
  for (int k = 0; k < 4; ++k) {
    out.put(static_cast<char>(v & 0xFFu));
    v >>= 8;
  }
}

inline void put_u64_le(std::ostream &out, std::uint64_t v) {
  for (int k = 0; k < 8; ++k) {
    out.put(static_cast<char>(v & 0xFFu));
    v >>= 8;
  }
}

inline void put_f64_le(std::ostream &out, double v) {
  put_u64_le(out, std::bit_cast<std::uint64_t>(v));
}

inline bool get_u8(std::istream &in, std::uint8_t &v) {
  const int c = in.get();
  if (c == std::char_traits<char>::eof()) {
    return false;
  }
  v = static_cast<std::uint8_t>(c);
  return true;
}

inline bool get_u16_le(std::istream &in, std::uint16_t &v) {
  std::uint8_t lo = 0;
  std::uint8_t hi = 0;
  if (!get_u8(in, lo) || !get_u8(in, hi)) {
    return false;
  }
  v = static_cast<std::uint16_t>(lo) | (static_cast<std::uint16_t>(hi) << 8);
  return true;
}

inline bool get_u32_le(std::istream &in, std::uint32_t &v) {
  v = 0;
  for (int k = 0; k < 4; ++k) {
    std::uint8_t b = 0;
    if (!get_u8(in, b)) {
      return false;
    }
    v |= static_cast<std::uint32_t>(b) << (8 * k);
  }
  return true;
}

inline bool get_u64_le(std::istream &in, std::uint64_t &v) {
  v = 0;
  for (int k = 0; k < 8; ++k) {
    std::uint8_t b = 0;
    if (!get_u8(in, b)) {
      return false;
    }
    v |= static_cast<std::uint64_t>(b) << (8 * k);
  }
  return true;
}

inline bool get_f64_le(std::istream &in, double &v) {
  std::uint64_t bits = 0;
  if (!get_u64_le(in, bits)) {
    return false;
  }
  v = std::bit_cast<double>(bits);
  return true;
}

} // namespace detail

[[nodiscard]] inline core::Status save_state(const State &s, std::ostream &out) {
  out.put('W');
  out.put('E');
  out.put('N');
  out.put('G');
  detail::put_u32_le(out, kStateFormatVersion);
  detail::put_f64_le(out, s.sim_time);
  detail::put_u64_le(out, s.step_index);

  const auto &gens = s.entities.generations();
  const auto &free = s.entities.free_list();
  detail::put_u32_le(out, static_cast<std::uint32_t>(gens.size()));
  for (std::uint32_t g : gens) {
    detail::put_u32_le(out, g);
  }
  detail::put_u32_le(out, static_cast<std::uint32_t>(free.size()));
  for (std::uint32_t f : free) {
    detail::put_u32_le(out, f);
  }

  const std::size_t cap = s.particles.capacity();
  detail::put_u32_le(out, static_cast<std::uint32_t>(cap));
  for (std::size_t i = 0; i < cap; ++i) {
    detail::put_u8(out, s.particles.present[i]);
    detail::put_f64_le(out, s.particles.pos_x[i]);
    detail::put_f64_le(out, s.particles.pos_y[i]);
    detail::put_f64_le(out, s.particles.vel_x[i]);
    detail::put_f64_le(out, s.particles.vel_y[i]);
  }
  const std::size_t sig_cap = s.signals.capacity();
  detail::put_u32_le(out, static_cast<std::uint32_t>(sig_cap));
  for (std::size_t i = 0; i < sig_cap; ++i) {
    detail::put_u8(out, s.signals.present[i]);
    detail::put_u8(out, s.signals.kind[i]);
    detail::put_u8(out, s.signals.quantity[i]);
    detail::put_u8(out, s.signals.running[i]);
    detail::put_u8(out, s.signals.op_count[i]);
    detail::put_f64_le(out, s.signals.freq[i]);
    detail::put_f64_le(out, s.signals.amp[i]);
    detail::put_f64_le(out, s.signals.offset[i]);
    detail::put_f64_le(out, s.signals.phase[i]);
    detail::put_f64_le(out, s.signals.duty[i]);
    detail::put_f64_le(out, s.signals.t0[i]);
    detail::put_f64_le(out, s.signals.value[i]);
    detail::put_u64_le(out, s.signals.seed[i]);
    detail::put_u64_le(out, s.signals.rng[i]);
    detail::put_u64_le(out, s.signals.serial[i]);
    for (std::size_t k = 0; k < signal::SignalParams::kMaxOps; ++k) {
      detail::put_u64_le(out, s.signals.ops[i * signal::SignalParams::kMaxOps + k]);
    }
    detail::put_u8(out, s.signals.expr_len[i]);
    for (std::size_t k = 0; k < signal::kMaxExprNodes; ++k) {
      detail::put_u8(out, s.signals.expr_op[i * signal::kMaxExprNodes + k]);
    }
    for (std::size_t k = 0; k < signal::kMaxExprNodes; ++k) {
      detail::put_f64_le(out, s.signals.expr_arg[i * signal::kMaxExprNodes + k]);
    }
    detail::put_u64_le(out, s.signals.table_off[i]);
    detail::put_u64_le(out, s.signals.table_len[i]);
    detail::put_u64_le(out, s.signals.lua_off[i]);
    detail::put_u64_le(out, s.signals.lua_len[i]);
  }
  detail::put_u32_le(out, static_cast<std::uint32_t>(s.signals.table_times.size()));
  for (double t : s.signals.table_times) {
    detail::put_f64_le(out, t);
  }
  for (double v : s.signals.table_values) {
    detail::put_f64_le(out, v);
  }
  detail::put_u32_le(out, static_cast<std::uint32_t>(s.signals.lua_text.size()));
  out.write(s.signals.lua_text.data(),
            static_cast<std::streamsize>(s.signals.lua_text.size()));
  if (!out) {
    return {core::StatusCode::kInternal};
  }
  // Sequential pool (v6): fixed-stride topology + Q + stimulus + members.
  // Derived fanouts/topo recomputed on load, never stored.
  for (std::uint16_t slot = 0; slot < logic::kMaxGates; ++slot) {
    const logic::Gate &g = s.netlist.gates[slot];
    detail::put_u8(out, g.live ? 1 : 0);
    detail::put_u8(out, static_cast<std::uint8_t>(g.kind));
    detail::put_u32_le(out, g.counter);
    detail::put_u8(out, g.input_count);
    detail::put_u8(out, g.ignored_mask);
    detail::put_u8(out, g.ignore_output ? 1 : 0);
    detail::put_u8(out, g.ignore_nq_output ? 1 : 0);
    for (std::size_t k = 0; k < logic::kMaxInputs; ++k) {
      detail::put_u16_le(out, g.drivers[k]);
    }
    for (std::size_t k = 0; k < logic::kMaxInputs; ++k) {
      detail::put_u8(out, s.netlist.driver_inv[static_cast<std::size_t>(slot) * logic::kMaxInputs + k]);
    }
    for (std::size_t k = 0; k < logic::kMaxInputs; ++k) {
      detail::put_u16_le(out, s.netlist.tie_rep[static_cast<std::size_t>(slot) * logic::kMaxInputs + k]);
    }
    detail::put_u64_le(out, g.propagation_delay_ps);
  }
  for (std::uint32_t c : s.netlist.next_counter) {
    detail::put_u32_le(out, c);
  }
  detail::put_u16_le(out, s.netlist.live_count);
  for (std::uint16_t slot = 0; slot < logic::kMaxGates; ++slot) {
    detail::put_u8(out, s.seq.valid[slot]);
    detail::put_u8(out, static_cast<std::uint8_t>(s.seq.q[slot]));
    detail::put_u64_le(out, s.seq.serial[slot]);
  }
  detail::put_u64_le(out, s.seq.next_serial);
  detail::put_u16_le(out, s.seq.live_count);
  detail::put_u32_le(out, static_cast<std::uint32_t>(s.seq_inputs.size()));
  for (logic::LogicValue v : s.seq_inputs) {
    detail::put_u8(out, static_cast<std::uint8_t>(v));
  }
  detail::put_u32_le(out, static_cast<std::uint32_t>(s.seq_members.size()));
  for (std::uint16_t m : s.seq_members) {
    detail::put_u16_le(out, m);
  }
  detail::put_u8(out, s.seq_members.empty() ? 1 : 0);
  detail::put_u32_le(out, static_cast<std::uint32_t>(s.seq_outputs.size()));
  for (logic::LogicValue v : s.seq_outputs) {
    detail::put_u8(out, static_cast<std::uint8_t>(v));
  }
  if (!out) {
    return {core::StatusCode::kInternal};
  }
  return core::Status::success();
}

[[nodiscard]] inline core::Status load_state(State &s, std::istream &in) {
  s.clear();
  auto invalid = [] {
    return core::Status{core::StatusCode::kInvalidArgument};
  };
  char magic[4] = {};
  in.read(magic, 4);
  if (!in || magic[0] != 'W' || magic[1] != 'E' || magic[2] != 'N' || magic[3] != 'G') {
    return invalid();
  }
  std::uint32_t version = 0;
  if (!detail::get_u32_le(in, version) || version < 1 || version > kStateFormatVersion) {
    return invalid();
  }
  State tmp;
  if (!detail::get_f64_le(in, tmp.sim_time) || !detail::get_u64_le(in, tmp.step_index)) {
    return invalid();
  }
  std::uint32_t entity_cap = 0;
  if (!detail::get_u32_le(in, entity_cap) || entity_cap > kMaxStateEntries) {
    return invalid();
  }
  std::vector<std::uint32_t> gens(entity_cap);
  for (std::uint32_t &g : gens) {
    if (!detail::get_u32_le(in, g)) {
      return invalid();
    }
  }
  std::uint32_t free_size = 0;
  if (!detail::get_u32_le(in, free_size) || free_size > entity_cap) {
    return invalid();
  }
  std::vector<std::uint32_t> free(free_size);
  std::vector<char> seen(entity_cap, 0);
  for (std::uint32_t &f : free) {
    if (!detail::get_u32_le(in, f) || f >= entity_cap || seen[f] != 0) {
      return invalid();
    }
    seen[f] = 1;
  }
  std::uint32_t part_cap = 0;
  if (!detail::get_u32_le(in, part_cap) || part_cap > kMaxStateEntries) {
    return invalid();
  }
  tmp.particles.pos_x.resize(part_cap);
  tmp.particles.pos_y.resize(part_cap);
  tmp.particles.vel_x.resize(part_cap);
  tmp.particles.vel_y.resize(part_cap);
  tmp.particles.present.resize(part_cap);
  tmp.particles.live_count = 0;
  for (std::uint32_t i = 0; i < part_cap; ++i) {
    std::uint8_t p = 0;
    if (!detail::get_u8(in, p) || p > 1) {
      return invalid();
    }
    tmp.particles.present[i] = p;
    tmp.particles.live_count += p;
    if (!detail::get_f64_le(in, tmp.particles.pos_x[i]) ||
        !detail::get_f64_le(in, tmp.particles.pos_y[i]) ||
        !detail::get_f64_le(in, tmp.particles.vel_x[i]) ||
        !detail::get_f64_le(in, tmp.particles.vel_y[i])) {
      return invalid();
    }
  }
  tmp.entities.restore(std::move(gens), std::move(free));
  if (version == 1) {
    s = std::move(tmp); // v1 has no signal section: signals stay empty
    return core::Status::success();
  }
  std::uint32_t sig_cap = 0;
  if (!detail::get_u32_le(in, sig_cap) || sig_cap > kMaxStateEntries) {
    return invalid();
  }
  tmp.signals.kind.resize(sig_cap);
  tmp.signals.quantity.resize(sig_cap);
  tmp.signals.freq.resize(sig_cap);
  tmp.signals.amp.resize(sig_cap);
  tmp.signals.offset.resize(sig_cap);
  tmp.signals.phase.resize(sig_cap);
  tmp.signals.duty.resize(sig_cap);
  tmp.signals.seed.resize(sig_cap);
  tmp.signals.rng.resize(sig_cap);
  tmp.signals.t0.resize(sig_cap);
  tmp.signals.value.resize(sig_cap);
  tmp.signals.running.resize(sig_cap);
  tmp.signals.op_count.resize(sig_cap);
  tmp.signals.serial.resize(sig_cap);
  tmp.signals.ops.resize(static_cast<std::size_t>(sig_cap) *
                         signal::SignalParams::kMaxOps);
  tmp.signals.expr_op.resize(static_cast<std::size_t>(sig_cap) * signal::kMaxExprNodes);
  tmp.signals.expr_arg.resize(static_cast<std::size_t>(sig_cap) * signal::kMaxExprNodes);
  tmp.signals.expr_len.resize(sig_cap);
  tmp.signals.table_off.resize(sig_cap);
  tmp.signals.table_len.resize(sig_cap);
  tmp.signals.lua_off.resize(sig_cap);
  tmp.signals.lua_len.resize(sig_cap);
  tmp.signals.present.resize(sig_cap);
  tmp.signals.live_count = 0;
  tmp.signals.next_serial = 0;
  for (std::uint32_t i = 0; i < sig_cap; ++i) {
    std::uint8_t p = 0;
    std::uint8_t kind = 0;
    std::uint8_t qty = 0;
    std::uint8_t run = 0;
    std::uint8_t ops = 0;
    if (!detail::get_u8(in, p) || p > 1 || !detail::get_u8(in, kind) ||
        kind > static_cast<std::uint8_t>(signal::SignalKind::kLua) ||
        !detail::get_u8(in, qty) ||
        qty > static_cast<std::uint8_t>(signal::QuantityId::kCurrent) ||
        !detail::get_u8(in, run) || run > 1) {
      return invalid();
    }
    if (version >= 3) {
      if (!detail::get_u8(in, ops) || ops > signal::SignalParams::kMaxOps) {
        return invalid();
      }
    }
    tmp.signals.present[i] = p;
    tmp.signals.live_count += p;
    tmp.signals.kind[i] = kind;
    tmp.signals.quantity[i] = qty;
    tmp.signals.running[i] = run;
    tmp.signals.op_count[i] = (version >= 3) ? ops : 0;
    if (!detail::get_f64_le(in, tmp.signals.freq[i]) ||
        !detail::get_f64_le(in, tmp.signals.amp[i]) ||
        !detail::get_f64_le(in, tmp.signals.offset[i]) ||
        !detail::get_f64_le(in, tmp.signals.phase[i]) ||
        !detail::get_f64_le(in, tmp.signals.duty[i]) ||
        !detail::get_f64_le(in, tmp.signals.t0[i]) ||
        !detail::get_f64_le(in, tmp.signals.value[i]) ||
        !detail::get_u64_le(in, tmp.signals.seed[i]) ||
        !detail::get_u64_le(in, tmp.signals.rng[i])) {
      return invalid();
    }
    if (version >= 3) {
      std::uint64_t serial = 0;
      if (!detail::get_u64_le(in, serial) || serial + 1 == 0) {
        return invalid(); // exhausted serial space: corrupt, never produced
      }
      tmp.signals.serial[i] = serial;
      if (serial >= tmp.signals.next_serial) {
        tmp.signals.next_serial = serial + 1;
      }
      for (std::size_t k = 0; k < signal::SignalParams::kMaxOps; ++k) {
        if (!detail::get_u64_le(
                in, tmp.signals.ops[i * signal::SignalParams::kMaxOps + k])) {
          return invalid();
        }
      }
    } else {
      // v2 has no serials/operands: synthesize serials as slot indices,
      // operand lists stay empty (sums did not exist in v2).
      tmp.signals.serial[i] = i;
      if (static_cast<std::uint64_t>(i) + 1 > tmp.signals.next_serial) {
        tmp.signals.next_serial = static_cast<std::uint64_t>(i) + 1;
      }
    }
    if (version >= 4) {
      std::uint8_t expr_len = 0;
      if (!detail::get_u8(in, expr_len) || expr_len > signal::kMaxExprNodes) {
        return invalid();
      }
      tmp.signals.expr_len[i] = expr_len;
      for (std::size_t k = 0; k < signal::kMaxExprNodes; ++k) {
        std::uint8_t op = 0;
        if (!detail::get_u8(in, op) ||
            op >= static_cast<std::uint8_t>(signal::ExprOp::kCount)) {
          return invalid();
        }
        tmp.signals.expr_op[i * signal::kMaxExprNodes + k] = op;
      }
      for (std::size_t k = 0; k < signal::kMaxExprNodes; ++k) {
        if (!detail::get_f64_le(in, tmp.signals.expr_arg[i * signal::kMaxExprNodes + k])) {
          return invalid();
        }
      }
      if (!detail::get_u64_le(in, tmp.signals.table_off[i]) ||
          !detail::get_u64_le(in, tmp.signals.table_len[i]) ||
          tmp.signals.table_len[i] > signal::kMaxTablePoints) {
        return invalid();
      }
      if (version >= 5) {
        if (!detail::get_u64_le(in, tmp.signals.lua_off[i]) ||
            !detail::get_u64_le(in, tmp.signals.lua_len[i]) ||
            tmp.signals.lua_len[i] > signal::kMaxLuaScriptBytes) {
          return invalid();
        }
      } else {
        // v4 predates scripted sources: no script ranges.
        tmp.signals.lua_off[i] = 0;
        tmp.signals.lua_len[i] = 0;
      }
      if (p != 0) {
        // Live slots re-validate the configure-time invariants: programs
        // must be stack-balanced (expressions) or empty, table slots must
        // carry a non-empty range (checked against the store below), and
        // Lua slots must carry a non-empty script range (ditto).
        signal::ExprProgram prog;
        prog.size = expr_len;
        for (std::size_t k = 0; k < signal::kMaxExprNodes; ++k) {
          prog.nodes[k].op = static_cast<signal::ExprOp>(
              tmp.signals.expr_op[i * signal::kMaxExprNodes + k]);
          prog.nodes[k].value = tmp.signals.expr_arg[i * signal::kMaxExprNodes + k];
        }
        const bool is_expr = kind == static_cast<std::uint8_t>(signal::SignalKind::kExpression);
        const bool is_table = kind == static_cast<std::uint8_t>(signal::SignalKind::kTable);
        const bool is_lua = kind == static_cast<std::uint8_t>(signal::SignalKind::kLua);
        if (is_expr && !signal::validate_program(prog)) {
          return invalid();
        }
        if (!is_expr && expr_len != 0) {
          return invalid();
        }
        if (is_table && tmp.signals.table_len[i] == 0) {
          return invalid();
        }
        if (!is_table &&
            (tmp.signals.table_len[i] != 0 || tmp.signals.table_off[i] != 0)) {
          return invalid();
        }
        if (is_lua && tmp.signals.lua_len[i] == 0) {
          return invalid();
        }
        if (!is_lua &&
            (tmp.signals.lua_len[i] != 0 || tmp.signals.lua_off[i] != 0)) {
          return invalid();
        }
      } else {
        // Dead slots (p == 0) retain their last live columns verbatim:
        // detach() only clears presence, and save_state writes every
        // column. Load accepts the retained payload after the size/opcode
        // bounds checked at read time above and the store/blob containment
        // checks below; no live-source invariant (stack balance, non-empty
        // range) applies. Retained data can never become live: every
        // configure/attach path overwrites the custom columns, and
        // next_serial advances past dead serials, so serial progression
        // and shared table/script ranges stay exact.
      }
    } else {
      // v3 and earlier predate custom sources: programs empty, no ranges.
      tmp.signals.expr_len[i] = 0;
      for (std::size_t k = 0; k < signal::kMaxExprNodes; ++k) {
        tmp.signals.expr_op[i * signal::kMaxExprNodes + k] = 0;
        tmp.signals.expr_arg[i * signal::kMaxExprNodes + k] = 0.0;
      }
      tmp.signals.table_off[i] = 0;
      tmp.signals.table_len[i] = 0;
      tmp.signals.lua_off[i] = 0;
      tmp.signals.lua_len[i] = 0;
    }
  }
  if (version >= 4) {
    std::uint32_t store_len = 0;
    if (!detail::get_u32_le(in, store_len) || store_len > kMaxStateEntries) {
      return invalid();
    }
    tmp.signals.table_times.resize(store_len);
    tmp.signals.table_values.resize(store_len);
    for (std::uint32_t k = 0; k < store_len; ++k) {
      if (!detail::get_f64_le(in, tmp.signals.table_times[k])) {
        return invalid();
      }
    }
    for (std::uint32_t k = 0; k < store_len; ++k) {
      if (!detail::get_f64_le(in, tmp.signals.table_values[k])) {
        return invalid();
      }
    }
    // Ranges must land inside the store; live table ranges must hold
    // finite, strictly increasing points (the configure-time invariant).
    for (std::uint32_t i = 0; i < sig_cap; ++i) {
      const std::uint64_t off = tmp.signals.table_off[i];
      const std::uint64_t len = tmp.signals.table_len[i];
      if (off > store_len || len > store_len - off) {
        return invalid();
      }
      if (tmp.signals.present[i] != 0 &&
          tmp.signals.kind[i] == static_cast<std::uint8_t>(signal::SignalKind::kTable)) {
        for (std::uint64_t k = 0; k < len; ++k) {
          const double t = tmp.signals.table_times[off + k];
          const double v = tmp.signals.table_values[off + k];
          if (!std::isfinite(t) || !std::isfinite(v)) {
            return invalid();
          }
          if (k > 0 && !(t > tmp.signals.table_times[off + k - 1])) {
            return invalid();
          }
        }
      }
    }
  }
  if (version >= 5) {
    // Script blob: opaque bytes (compile-checked where Lua exists, never
    // here); ranges validated against the blob like table ranges above.
    std::uint32_t blob_len = 0;
    if (!detail::get_u32_le(in, blob_len) || blob_len > kMaxStateEntries) {
      return invalid();
    }
    tmp.signals.lua_text.resize(blob_len);
    if (blob_len > 0) {
      in.read(tmp.signals.lua_text.data(), static_cast<std::streamsize>(blob_len));
      if (!in) {
        return invalid();
      }
    }
    for (std::uint32_t i = 0; i < sig_cap; ++i) {
      const std::uint64_t off = tmp.signals.lua_off[i];
      const std::uint64_t len = tmp.signals.lua_len[i];
      if (off > blob_len || len > blob_len - off) {
        return invalid();
      }
    }
  }
  // Sequential pool (v6 only). v1–v5 files load with an empty pool.
  if (version >= 6) {
    auto is_known_kind = [](std::uint8_t b) noexcept {
      return b <= 7 || b == 16 || b == 17 || b == 18 || b == 19 || b == 20;
    };
    tmp.netlist = logic::Netlist{};
    for (std::uint16_t slot = 0; slot < logic::kMaxGates; ++slot) {
      std::uint8_t live = 0;
      std::uint8_t kind = 0;
      std::uint32_t counter = 0;
      std::uint8_t input_count = 0;
      std::uint8_t ignored_mask = 0;
      std::uint8_t ignore_out = 0;
      std::uint8_t ignore_nq = 0;
      if (!detail::get_u8(in, live) || live > 1 || !detail::get_u8(in, kind) ||
          !is_known_kind(kind) || !detail::get_u32_le(in, counter) ||
          !detail::get_u8(in, input_count) || input_count < 1 ||
          input_count > logic::kMaxInputs || !detail::get_u8(in, ignored_mask) ||
          !detail::get_u8(in, ignore_out) || ignore_out > 1 ||
          !detail::get_u8(in, ignore_nq) || ignore_nq > 1) {
        return invalid();
      }
      if (live != 0) {
        if (counter == 0) {
          return invalid();
        }
        if (!logic::valid_fanin(static_cast<logic::GateKind>(kind), input_count)) {
          return invalid();
        }
      }
      if ((ignored_mask >> input_count) != 0) {
        return invalid();
      }
      logic::Gate &g = tmp.netlist.gates[slot];
      g.live = live != 0;
      g.kind = static_cast<logic::GateKind>(kind);
      g.counter = counter;
      g.input_count = input_count;
      g.ignored_mask = ignored_mask;
      g.ignore_output = ignore_out != 0;
      g.ignore_nq_output = ignore_nq != 0;
      for (std::size_t k = 0; k < logic::kMaxInputs; ++k) {
        std::uint16_t d = 0;
        if (!detail::get_u16_le(in, d) ||
            (d != logic::kNoDriver && d >= logic::kMaxGates)) {
          return invalid();
        }
        g.drivers[k] = d;
      }
      for (std::size_t k = 0; k < logic::kMaxInputs; ++k) {
        std::uint8_t inv = 0;
        if (!detail::get_u8(in, inv) || inv > 1) {
          return invalid();
        }
        tmp.netlist.driver_inv[static_cast<std::size_t>(slot) * logic::kMaxInputs + k] = inv;
      }
      for (std::size_t k = 0; k < logic::kMaxInputs; ++k) {
        std::uint16_t rep = 0;
        if (!detail::get_u16_le(in, rep) ||
            rep > logic::kMaxGates * logic::kMaxInputs) {
          return invalid();
        }
        tmp.netlist.tie_rep[static_cast<std::size_t>(slot) * logic::kMaxInputs + k] = rep;
      }
      std::uint64_t delay = 0;
      if (!detail::get_u64_le(in, delay)) {
        return invalid();
      }
      g.propagation_delay_ps = delay;
    }
    for (std::size_t k = 0; k < tmp.netlist.next_counter.size(); ++k) {
      std::uint32_t c = 0;
      if (!detail::get_u32_le(in, c) || c == 0) {
        return invalid();
      }
      tmp.netlist.next_counter[k] = c;
    }
    std::uint16_t nl_live = 0;
    if (!detail::get_u16_le(in, nl_live) || nl_live > logic::kMaxGates) {
      return invalid();
    }
    std::uint16_t counted_live = 0;
    for (std::uint16_t slot = 0; slot < logic::kMaxGates; ++slot) {
      if (tmp.netlist.gates[slot].live) {
        ++counted_live;
      }
    }
    if (counted_live != nl_live) {
      return invalid();
    }
    tmp.netlist.live_count = nl_live;
    // Wired drivers must land on live slots; tied pins must be live,
    // unwired, non-ignored with star-consistent reps; NQ polarity only
    // from live sequential sources.
    for (std::uint16_t slot = 0; slot < logic::kMaxGates; ++slot) {
      const logic::Gate &g = tmp.netlist.gates[slot];
      if (!g.live) {
        continue;
      }
      for (std::uint8_t i = 0; i < g.input_count; ++i) {
        const std::size_t flat =
            static_cast<std::size_t>(slot) * logic::kMaxInputs + i;
        const std::uint16_t drv = g.drivers[i];
        if (drv != logic::kNoDriver && !tmp.netlist.gates[drv].live) {
          return invalid();
        }
        if (tmp.netlist.driver_inv[flat] != 0) {
          if (drv == logic::kNoDriver || !logic::is_sequential_kind(tmp.netlist.gates[drv].kind)) {
            return invalid();
          }
        }
        const std::uint16_t rep = tmp.netlist.tie_rep[flat];
        if (rep != 0) {
          if (drv != logic::kNoDriver || (g.ignored_mask & (1u << i)) != 0) {
            return invalid();
          }
          const std::size_t leader = static_cast<std::size_t>(rep - 1u);
          if (leader >= logic::kMaxGates * logic::kMaxInputs ||
              tmp.netlist.tie_rep[leader] != rep) {
            return invalid();
          }
          const std::uint16_t ls = static_cast<std::uint16_t>(leader / logic::kMaxInputs);
          const std::uint8_t li = static_cast<std::uint8_t>(leader % logic::kMaxInputs);
          const logic::Gate &lg = tmp.netlist.gates[ls];
          if (!lg.live || li >= lg.input_count || lg.drivers[li] != logic::kNoDriver ||
              (lg.ignored_mask & (1u << li)) != 0) {
            return invalid();
          }
        }
      }
    }
    // Recompute derived data (fanouts, topo), never stored.
    for (std::uint16_t slot = 0; slot < logic::kMaxGates; ++slot) {
      tmp.netlist.gates[slot].output_fanout = 0;
      tmp.netlist.gates[slot].nq_fanout = 0;
    }
    for (std::uint16_t slot = 0; slot < logic::kMaxGates; ++slot) {
      const logic::Gate &g = tmp.netlist.gates[slot];
      if (!g.live) {
        continue;
      }
      for (std::uint8_t i = 0; i < g.input_count; ++i) {
        const std::uint16_t d = g.drivers[i];
        if (d == logic::kNoDriver) {
          continue;
        }
        const std::size_t flat =
            static_cast<std::size_t>(slot) * logic::kMaxInputs + i;
        if (tmp.netlist.driver_inv[flat] != 0) {
          ++tmp.netlist.gates[d].nq_fanout;
        } else {
          ++tmp.netlist.gates[d].output_fanout;
        }
      }
    }
    tmp.netlist.dirty = true;
    tmp.netlist.topo_size = 0;
    // Clocked state: valid slots must match live sequential gates exactly.
    tmp.seq = logic::SeqState{};
    std::uint64_t max_serial = 0;
    bool have_serial = false;
    for (std::uint16_t slot = 0; slot < logic::kMaxGates; ++slot) {
      std::uint8_t valid = 0;
      std::uint8_t q = 0;
      std::uint64_t serial = 0;
      if (!detail::get_u8(in, valid) || valid > 1 || !detail::get_u8(in, q) || q > 1 ||
          !detail::get_u64_le(in, serial)) {
        return invalid();
      }
      const bool want = tmp.netlist.gates[slot].live &&
                        logic::is_sequential_kind(tmp.netlist.gates[slot].kind);
      if ((valid != 0) != want) {
        return invalid();
      }
      if (valid != 0) {
        tmp.seq.valid[slot] = 1;
        tmp.seq.q[slot] = (q != 0) ? logic::LogicValue::kHigh : logic::LogicValue::kLow;
        tmp.seq.serial[slot] = serial;
        for (std::uint16_t o = 0; o < slot; ++o) {
          if (tmp.seq.valid[o] != 0 && tmp.seq.serial[o] == serial) {
            return invalid();
          }
        }
        if (!have_serial || serial > max_serial) {
          max_serial = serial;
          have_serial = true;
        }
      }
    }
    std::uint64_t next_serial = 0;
    std::uint16_t seq_live = 0;
    if (!detail::get_u64_le(in, next_serial) || !detail::get_u16_le(in, seq_live) ||
        seq_live > logic::kMaxGates) {
      return invalid();
    }
    std::uint16_t counted_seq = 0;
    for (std::uint16_t slot = 0; slot < logic::kMaxGates; ++slot) {
      if (tmp.seq.valid[slot] != 0) {
        ++counted_seq;
      }
    }
    if (counted_seq != seq_live) {
      return invalid();
    }
    if (have_serial && next_serial <= max_serial) {
      return invalid();
    }
    tmp.seq.next_serial = next_serial;
    tmp.seq.live_count = seq_live;
    // Stimulus: length must match the derived block interface, Low/High only.
    std::uint32_t stim_len = 0;
    if (!detail::get_u32_le(in, stim_len) || stim_len > kMaxStateEntries) {
      return invalid();
    }
    if (stim_len != logic::block_input_count(tmp.netlist)) {
      return invalid();
    }
    tmp.seq_inputs.clear();
    tmp.seq_inputs.reserve(stim_len);
    for (std::uint32_t k = 0; k < stim_len; ++k) {
      std::uint8_t b = 0;
      if (!detail::get_u8(in, b) || b > 1) {
        return invalid();
      }
      tmp.seq_inputs.push_back((b != 0) ? logic::LogicValue::kHigh : logic::LogicValue::kLow);
    }
    // Members: live edge-flop (DFF/JK/T) slots, no duplicates; step_all
    // consistent (all iff empty).
    std::uint32_t mem_len = 0;
    if (!detail::get_u32_le(in, mem_len) || mem_len > logic::kMaxGates) {
      return invalid();
    }
    tmp.seq_members.clear();
    tmp.seq_members.reserve(mem_len);
    for (std::uint32_t k = 0; k < mem_len; ++k) {
      std::uint16_t m = 0;
      if (!detail::get_u16_le(in, m) || m >= logic::kMaxGates) {
        return invalid();
      }
      const logic::Gate &g = tmp.netlist.gates[m];
      if (!g.live) {
        return invalid();
      }
      const bool edge_kind = g.kind == logic::GateKind::kDff || g.kind == logic::GateKind::kJk ||
                             g.kind == logic::GateKind::kT;
      if (!edge_kind) {
        return invalid();
      }
      for (std::uint16_t seen : tmp.seq_members) {
        if (seen == m) {
          return invalid();
        }
      }
      tmp.seq_members.push_back(m);
    }
    std::uint8_t step_all = 0;
    if (!detail::get_u8(in, step_all) || step_all > 1) {
      return invalid();
    }
    if ((step_all != 0) == (!tmp.seq_members.empty())) {
      return invalid();
    }
    // Retained settled outputs: length matches the derived interface.
    std::uint32_t out_len = 0;
    if (!detail::get_u32_le(in, out_len) || out_len > kMaxStateEntries) {
      return invalid();
    }
    if (out_len != logic::block_output_count(tmp.netlist)) {
      return invalid();
    }
    tmp.seq_outputs.clear();
    tmp.seq_outputs.reserve(out_len);
    for (std::uint32_t k = 0; k < out_len; ++k) {
      std::uint8_t b = 0;
      if (!detail::get_u8(in, b) || b > 1) {
        return invalid();
      }
      tmp.seq_outputs.push_back((b != 0) ? logic::LogicValue::kHigh : logic::LogicValue::kLow);
    }
  } else {
    tmp.netlist = logic::Netlist{};
    tmp.seq = logic::SeqState{};
    tmp.seq_inputs.clear();
    tmp.seq_members.clear();
    tmp.seq_outputs.clear();
  }
  // Sum invariants (configure-time rule, re-checked so a crafted or
  // hand-edited checkpoint cannot bypass it): live sums need 1..kMaxOps
  // operands, each resolving to a live non-sum source with the sum's own
  // quantity (QuantityId equality only — generic over kQuantities); live
  // non-sums carry no operand count. Dead slots keep stale columns and are
  // skipped. v2 predates sums, so any live kSum there fails (empty list).
  for (std::uint32_t i = 0; i < sig_cap; ++i) {
    if (tmp.signals.present[i] == 0) {
      continue;
    }
    const bool is_sum = tmp.signals.kind[i] == static_cast<std::uint8_t>(signal::SignalKind::kSum);
    if (!is_sum) {
      if (tmp.signals.op_count[i] != 0) {
        return invalid();
      }
      continue;
    }
    if (tmp.signals.op_count[i] < 1 || tmp.signals.op_count[i] > signal::SignalParams::kMaxOps) {
      return invalid();
    }
    const auto sum_q = static_cast<signal::QuantityId>(tmp.signals.quantity[i]);
    if (!signal::is_valid_quantity(sum_q)) {
      return invalid();
    }
    for (std::uint8_t k = 0; k < tmp.signals.op_count[i]; ++k) {
      const std::uint64_t want = tmp.signals.ops[i * signal::SignalParams::kMaxOps + k];
      const auto oslot = find_serial(tmp.signals, want);
      if (!oslot.has_value()) {
        return invalid();
      }
      if (tmp.signals.kind[*oslot] == static_cast<std::uint8_t>(signal::SignalKind::kSum)) {
        return invalid();
      }
      if (tmp.signals.present[*oslot] == 0) {
        return invalid();
      }
      const auto op_q = static_cast<signal::QuantityId>(tmp.signals.quantity[*oslot]);
      if (!signal::is_valid_quantity(op_q) || op_q != sum_q) {
        return invalid();
      }
    }
  }
  s = std::move(tmp);
  return core::Status::success();
}

} // namespace wengine::world
