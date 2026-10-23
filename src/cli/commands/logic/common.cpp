#include "common.hpp"

#include "wengine/logic/eval.hpp"
#include "wengine/logic/types.hpp"
#include "wengine/logic/yaml_store.hpp"

#include <fstream>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::logic {

using wengine::logic::BlockPin;
using wengine::logic::GateKind;
using wengine::logic::kMaxGates;
using wengine::logic::kMaxInputs;
using wengine::logic::kNoDriver;
using wengine::logic::Netlist;

namespace support {

void push_matches(std::string_view partial, std::span<const std::string_view> options, std::vector<std::string> &out) {
  for (std::string_view o : options) {
    if (o.substr(0, partial.size()) == partial) {
      out.emplace_back(o);
    }
  }
}

[[nodiscard]] bool starts_with(std::string_view text, std::string_view prefix) noexcept {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] std::string gate_name(const Netlist &nl, std::uint16_t slot) {
  std::string s(wengine::logic::gate_kind_name(nl.gates[slot].kind));
  s += std::to_string(nl.gates[slot].counter);
  return s;
}

[[nodiscard]] std::string pin_text(const Netlist &nl, std::uint16_t slot, bool is_output, std::uint8_t input) {
  std::string s = gate_name(nl, slot);
  s += '.';
  if (is_output) {
    s += 'Y';
  } else {
    s += std::to_string(static_cast<unsigned>(input) + 1u);
  }
  return s;
}

} // namespace support

namespace support {

void collect_gate_names(const Netlist &nl, std::string_view partial, std::vector<std::string> &out) {
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (!nl.gates[s].live) {
      continue;
    }
    std::string name = gate_name(nl, s);
    if (name.size() >= partial.size() && name.substr(0, partial.size()) == partial) {
      out.push_back(name);
    }
  }
}

void collect_pins(const Netlist &nl, bool want_out, bool want_in, std::string_view partial, std::vector<std::string> &out) {
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const auto &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    if (want_out) {
      std::string p = pin_text(nl, s, true, 0);
      if (p.size() >= partial.size() && p.substr(0, partial.size()) == partial) {
        out.push_back(p);
      }
    }
    if (want_in) {
      for (std::uint8_t i = 0; i < g.input_count; ++i) {
        std::string p = pin_text(nl, s, false, i);
        if (p.size() >= partial.size() && p.substr(0, partial.size()) == partial) {
          out.push_back(p);
        }
      }
    }
  }
}

void collect_block_inputs(const Netlist &nl, std::string_view partial, std::vector<std::string> &out) {
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const auto &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (g.drivers[i] != kNoDriver) {
        continue;
      }
      if ((g.ignored_mask & static_cast<std::uint8_t>(1u << i)) != 0) {
        continue;
      }
      std::string p = pin_text(nl, s, false, i);
      if (p.size() >= partial.size() && p.substr(0, partial.size()) == partial) {
        out.push_back(p);
      }
    }
  }
}

void interface_pins(const Netlist &nl, std::vector<BlockPin> &in_pins, std::vector<BlockPin> &out_pins) {
  const std::size_t nin = wengine::logic::block_input_count(nl);
  const std::size_t nout = wengine::logic::block_output_count(nl);
  in_pins.assign(nin, BlockPin{});
  out_pins.assign(nout, BlockPin{});
  std::size_t nin_got = nin;
  std::size_t nout_got = nout;
  const core::Status st = wengine::logic::block_interface(nl, in_pins.data(), in_pins.size(), &nin_got, out_pins.data(), out_pins.size(), &nout_got);
  (void)st; // sizes are exact by construction
}

core::Status write_truth_csv(Netlist &nl, std::uint64_t rows, const std::vector<BlockPin> &in_pins,
                             const std::vector<BlockPin> &out_pins, std::ostream &os) {
  // Header: input pins then output pins.
  bool first = true;
  for (const BlockPin &p : in_pins) {
    if (!first) {
      os << ',';
    }
    first = false;
    os << pin_text(nl, p.slot, false, p.input);
  }
  for (const BlockPin &p : out_pins) {
    if (!first) {
      os << ',';
    }
    first = false;
    os << pin_text(nl, p.slot, true, 0);
  }
  os << '\n';
  if (!os) {
    return core::Status::failure(core::StatusCode::kInternal);
  }
  const std::size_t nout = out_pins.size();
  std::vector<std::uint64_t> scratch(kMaxGates, 0);
  std::vector<std::uint64_t> words(nout == 0 ? 1 : nout, 0);
  const std::uint64_t nwords = (rows + 63ULL) / 64ULL;
  for (std::uint64_t w = 0; w < nwords; ++w) {
    std::span<std::uint64_t> out_span = nout == 0 ? std::span<std::uint64_t>{} : std::span<std::uint64_t>{words.data(), nout};
    const core::Status st = wengine::logic::eval_block_word(nl, w, std::span<std::uint64_t>{scratch.data(), scratch.size()}, out_span);
    if (!st.ok()) {
      return st;
    }
    for (std::uint64_t b = 0; b < 64; ++b) {
      const std::uint64_t row = w * 64ULL + b;
      if (row >= rows) {
        break;
      }
      bool col_first = true;
      for (std::size_t j = 0; j < in_pins.size(); ++j) {
        if (!col_first) {
          os << ',';
        }
        col_first = false;
        os << ((((row >> j) & 1ULL) != 0ULL) ? '1' : '0');
      }
      for (std::size_t o = 0; o < nout; ++o) {
        if (!col_first) {
          os << ',';
        }
        col_first = false;
        os << ((((words[o] >> b) & 1ULL) != 0ULL) ? '1' : '0');
      }
      os << '\n';
      if (!os) {
        return core::Status::failure(core::StatusCode::kInternal);
      }
    }
  }
  return core::Status::success();
}

core::Status write_truth_pretty(Netlist &nl, std::uint64_t rows, std::uint64_t total_rows,
                                const std::vector<BlockPin> &in_pins, const std::vector<BlockPin> &out_pins,
                                std::ostream &os) {
  const std::size_t nin = in_pins.size();
  const std::size_t nout = out_pins.size();
  const std::size_t ncols = nin + nout;
  if (ncols == 0) {
    os << "(empty block: no interface pins, 1 row)\n";
    if (!os) {
      return core::Status::failure(core::StatusCode::kInternal);
    }
    return core::Status::success();
  }
  std::vector<std::string> headers;
  headers.reserve(ncols);
  for (const BlockPin &p : in_pins) {
    headers.push_back(pin_text(nl, p.slot, false, p.input));
  }
  for (const BlockPin &p : out_pins) {
    headers.push_back(pin_text(nl, p.slot, true, 0));
  }
  std::vector<std::size_t> widths;
  widths.reserve(ncols);
  for (const std::string &h : headers) {
    widths.push_back(h.size());
  }
  for (std::size_t i = 0; i < ncols; ++i) {
    if (i != 0) {
      os << " | ";
    }
    os << headers[i];
  }
  os << '\n';
  for (std::size_t i = 0; i < ncols; ++i) {
    if (i != 0) {
      os << "-+-";
    }
    os << std::string(widths[i], '-');
  }
  os << '\n';
  if (!os) {
    return core::Status::failure(core::StatusCode::kInternal);
  }
  std::vector<std::uint64_t> scratch(kMaxGates, 0);
  std::vector<std::uint64_t> words(nout == 0 ? 1 : nout, 0);
  const std::uint64_t nwords = (rows + 63ULL) / 64ULL;
  for (std::uint64_t w = 0; w < nwords; ++w) {
    std::span<std::uint64_t> out_span = nout == 0 ? std::span<std::uint64_t>{} : std::span<std::uint64_t>{words.data(), nout};
    const core::Status st = wengine::logic::eval_block_word(nl, w, std::span<std::uint64_t>{scratch.data(), scratch.size()}, out_span);
    if (!st.ok()) {
      return st;
    }
    for (std::uint64_t b = 0; b < 64; ++b) {
      const std::uint64_t row = w * 64ULL + b;
      if (row >= rows) {
        break;
      }
      for (std::size_t c = 0; c < ncols; ++c) {
        if (c != 0) {
          os << " | ";
        }
        const char bit = (c < nin) ? ((((row >> c) & 1ULL) != 0ULL) ? '1' : '0')
                                   : ((((words[c - nin] >> b) & 1ULL) != 0ULL) ? '1' : '0');
        const std::size_t width = widths[c];
        const std::size_t left = (width - 1) / 2;
        const std::size_t right = width - 1 - left;
        os << std::string(left, ' ') << bit << std::string(right, ' ');
      }
      os << '\n';
      if (!os) {
        return core::Status::failure(core::StatusCode::kInternal);
      }
    }
  }
  if (rows < total_rows) {
    os << '(' << rows << " of " << total_rows << " rows shown; use --limit N or --csv <path> for more)\n";
    if (!os) {
      return core::Status::failure(core::StatusCode::kInternal);
    }
  }
  return core::Status::success();
}

void save_yaml(std::ostream &os, const Netlist &nl) {
  // Canonical bytes live in the versioned store (src/logic/yaml_store.cpp,
  // the single TU allowed to know yaml-cpp exists); this CLI wrapper only
  // streams them out. Byte-identical to the LOGIC-5 layout by construction.
  std::string text;
  wengine::logic::yaml::save_text(nl, text);
  os << text;
}

[[nodiscard]] std::size_t wire_count(const Netlist &nl) noexcept {
  std::size_t n = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const auto &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (g.drivers[i] != kNoDriver) {
        ++n;
      }
    }
  }
  return n;
}

void complete_from_value(const Netlist &nl, std::string_view val, std::vector<std::string> &out) {
  const std::size_t eq = val.find('=');
  if (eq == std::string_view::npos) {
    std::vector<std::string> pins;
    collect_block_inputs(nl, val, pins);
    // Exact pin hit: offer the two valued forms instead of the bare pin.
    bool exact = false;
    for (const std::string &p : pins) {
      if (p == val) {
        exact = true;
        break;
      }
    }
    if (exact) {
      out.emplace_back(std::string(val) + "=0");
      out.emplace_back(std::string(val) + "=1");
      return;
    }
    for (std::string &p : pins) {
      out.push_back(p);
    }
    return;
  }
  const std::string_view pin = val.substr(0, eq);
  const std::string_view bit = val.substr(eq + 1);
  std::vector<std::string> pins;
  collect_block_inputs(nl, pin, pins);
  bool known = false;
  for (const std::string &p : pins) {
    if (p == pin) {
      known = true;
      break;
    }
  }
  if (!known) {
    return;
  }
  if (std::string_view("0").substr(0, bit.size()) == bit) {
    out.emplace_back(std::string(pin) + "=0");
  }
  if (std::string_view("1").substr(0, bit.size()) == bit) {
    out.emplace_back(std::string(pin) + "=1");
  }
}

} // namespace support

namespace support {

namespace yaml {

bool load_text(const std::string &text, Netlist &out, std::string &err) {
  // Strict v1 loader lives in the versioned store
  // (src/logic/yaml_store.cpp, the single TU allowed to know yaml-cpp
  // exists); this CLI wrapper only forwards. Atomic: `out` is
  // untouched on failure, so `load` keeps the workspace on error.
  return wengine::logic::yaml::load_text(text, out, err);
}

} // namespace yaml

Schematic layout_schematic(Netlist &nl) {
  Schematic sc;
  if (nl.live_count == 0) {
    sc.width = 0;
    sc.height = 0;
    return sc;
  }
  int max_level = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (nl.gates[s].live && nl.topo_level[s] > max_level) {
      max_level = nl.topo_level[s];
    }
  }
  sc.levels = max_level + 1;
  constexpr int kColW = 150;
  constexpr int kBoxW = 110;
  constexpr int kBoxH = 40;
  constexpr int kGapY = 30;
  constexpr int kMargin = 30;
  std::vector<int> per_level(static_cast<std::size_t>(sc.levels), 0);
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (!nl.gates[s].live) {
      continue;
    }
    const int lvl = nl.topo_level[s];
    const int idx = per_level[static_cast<std::size_t>(lvl)]++;
    Placed p;
    p.slot = s;
    p.level = lvl;
    p.x = kMargin + lvl * kColW + (kColW - kBoxW) / 2;
    p.y = kMargin + idx * (kBoxH + kGapY);
    sc.gates.push_back(p);
  }
  int max_count = 1;
  for (int c : per_level) {
    if (c > max_count) {
      max_count = c;
    }
  }
  sc.width = kMargin * 2 + sc.levels * kColW;
  sc.height = kMargin * 2 + max_count * (kBoxH + kGapY);
  return sc;
}

void draw_ascii(std::ostream &os, const Netlist &nl, const Schematic &sc) {
  os << "schematic (gates=" << nl.live_count << ", levels=" << sc.levels << "):\n";
  if (nl.live_count == 0) {
    os << "  (empty)\n";
    return;
  }
  for (int lvl = 0; lvl < sc.levels; ++lvl) {
    os << "  L" << lvl << ":";
    for (const Placed &p : sc.gates) {
      if (p.level == lvl) {
        os << ' ' << gate_name(nl, p.slot);
      }
    }
    os << '\n';
  }
  std::size_t nw = wire_count(nl);
  os << "wires (" << nw << "):\n";
  if (nw == 0) {
    os << "  (none)\n";
    return;
  }
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const auto &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      const std::uint16_t drv = g.drivers[i];
      if (drv == kNoDriver) {
        continue;
      }
      os << "  " << pin_text(nl, drv, true, 0) << " -> " << pin_text(nl, s, false, i) << '\n';
    }
  }
}

bool write_bmp(const std::string &path, const Netlist &nl, const Schematic &sc, int &w_out, int &h_out, std::string &err) {
  constexpr int kBoxW = 110;
  constexpr int kBoxH = 40;
  const int w = sc.width;
  const int h = sc.height;
  w_out = w;
  h_out = h;
  std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3, 255);
  auto set_px = [&](int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    if (x < 0 || y < 0 || x >= w || y >= h) {
      return;
    }
    std::uint8_t *p = &px[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 3];
    p[0] = r;
    p[1] = g;
    p[2] = b;
  };
  auto hline = [&](int x0, int x1, int y) {
    if (x0 > x1) {
      const int t = x0;
      x0 = x1;
      x1 = t;
    }
    for (int x = x0; x <= x1; ++x) {
      set_px(x, y, 0, 0, 0);
      set_px(x, y + 1, 0, 0, 0);
    }
  };
  auto vline = [&](int x, int y0, int y1) {
    if (y0 > y1) {
      const int t = y0;
      y0 = y1;
      y1 = t;
    }
    for (int y = y0; y <= y1; ++y) {
      set_px(x, y, 0, 0, 0);
      set_px(x + 1, y, 0, 0, 0);
    }
  };
  // Wires first (under the boxes): Manhattan dogleg through the mid column.
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const auto &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    const Placed *dst = nullptr;
    for (const Placed &p : sc.gates) {
      if (p.slot == s) {
        dst = &p;
        break;
      }
    }
    if (dst == nullptr) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      const std::uint16_t drv = g.drivers[i];
      if (drv == kNoDriver) {
        continue;
      }
      const Placed *src = nullptr;
      for (const Placed &p : sc.gates) {
        if (p.slot == drv) {
          src = &p;
          break;
        }
      }
      if (src == nullptr) {
        continue;
      }
      const int n = g.input_count;
      const int y0 = src->y + kBoxH / 2;
      const int y1 = dst->y + (static_cast<int>(i) + 1) * kBoxH / (n + 1);
      const int x0 = src->x + kBoxW;
      const int x1 = dst->x;
      const int xm = (x0 + x1) / 2;
      hline(x0, xm, y0);
      vline(xm, y0, y1);
      hline(xm, x1, y1);
    }
  }
  // Boxes: gray fill, black border.
  for (const Placed &p : sc.gates) {
    for (int y = p.y; y < p.y + kBoxH; ++y) {
      for (int x = p.x; x < p.x + kBoxW; ++x) {
        set_px(x, y, 200, 200, 200);
      }
    }
    for (int x = p.x; x < p.x + kBoxW; ++x) {
      set_px(x, p.y, 0, 0, 0);
      set_px(x, p.y + 1, 0, 0, 0);
      set_px(x, p.y + kBoxH - 2, 0, 0, 0);
      set_px(x, p.y + kBoxH - 1, 0, 0, 0);
    }
    for (int y = p.y; y < p.y + kBoxH; ++y) {
      set_px(p.x, y, 0, 0, 0);
      set_px(p.x + 1, y, 0, 0, 0);
      set_px(p.x + kBoxW - 2, y, 0, 0, 0);
      set_px(p.x + kBoxW - 1, y, 0, 0, 0);
    }
  }
  const std::size_t stride = (static_cast<std::size_t>(w) * 3 + 3) & ~std::size_t(3);
  const std::size_t img = stride * static_cast<std::size_t>(h);
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    err = "cannot open file '" + path + "'";
    return false;
  }
  std::uint8_t hdr[54] = {};
  hdr[0] = 'B';
  hdr[1] = 'M';
  auto put32 = [&](int off, std::uint32_t v) {
    hdr[off] = static_cast<std::uint8_t>(v & 0xFFu);
    hdr[off + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    hdr[off + 2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
    hdr[off + 3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
  };
  auto put16 = [&](int off, std::uint32_t v) {
    hdr[off] = static_cast<std::uint8_t>(v & 0xFFu);
    hdr[off + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
  };
  put32(2, static_cast<std::uint32_t>(54 + img));
  put32(10, 54);
  put32(14, 40);
  put32(18, static_cast<std::uint32_t>(w));
  put32(22, static_cast<std::uint32_t>(h));
  put16(26, 1);
  put16(28, 24);
  put32(34, static_cast<std::uint32_t>(img));
  f.write(reinterpret_cast<const char *>(hdr), sizeof(hdr));
  std::vector<std::uint8_t> row(stride, 0);
  for (int y = h - 1; y >= 0; --y) {
    for (int x = 0; x < w; ++x) {
      const std::uint8_t *p = &px[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 3];
      row[static_cast<std::size_t>(x) * 3 + 0] = p[2];
      row[static_cast<std::size_t>(x) * 3 + 1] = p[1];
      row[static_cast<std::size_t>(x) * 3 + 2] = p[0];
    }
    f.write(reinterpret_cast<const char *>(row.data()), static_cast<std::streamsize>(stride));
  }
  f.close();
  if (!f) {
    err = "cannot write file '" + path + "'";
    return false;
  }
  return true;
}

[[nodiscard]] bool has_bmp_suffix(std::string_view path) noexcept {
  if (path.size() < 4) {
    return false;
  }
  const std::string_view suf = path.substr(path.size() - 4);
  return suf == ".bmp" || suf == ".BMP";
}

} // namespace support

} // namespace wengine::cli::logic
