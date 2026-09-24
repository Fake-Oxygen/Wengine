#pragma once

// Versioned binary persistence for world::State (deterministic, no deps).
//
// Format v5, all integers little-endian, doubles as IEEE-754 bits:
//   'W' 'E' 'N' 'G', u32 version (= 5)
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

inline constexpr std::uint32_t kStateFormatVersion = 5;
// Sanity cap per count field: rejects corrupt files before huge allocs.
inline constexpr std::uint32_t kMaxStateEntries = 1u << 24;

namespace detail {

inline void put_u8(std::ostream &out, std::uint8_t v) {
  out.put(static_cast<char>(v));
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
  if (!detail::get_u32_le(in, version) ||
      (version != 1 && version != 2 && version != 3 && version != 4 &&
       version != kStateFormatVersion)) {
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
      } else if (expr_len != 0 || tmp.signals.table_len[i] != 0 ||
                 tmp.signals.table_off[i] != 0 || tmp.signals.lua_len[i] != 0 ||
                 tmp.signals.lua_off[i] != 0) {
        return invalid(); // dead slots carry no program or range
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
  s = std::move(tmp);
  return core::Status::success();
}

} // namespace wengine::world
