#pragma once

/**
 * @file
 * Netlist text adapters (CLI/YAML text path) of the logic pipeline:
 * display-name and pin-spelling entry points over the mutation seam.
 * Split from netlist.hpp (F1) so the mutation header stays under 1k
 * lines; behavior is unchanged.
 *
 * Depends on netlist.hpp (mutation + Pin vocabulary) one-directionally;
 * netlist.hpp includes this file at its end for backward compatibility.
 * Logic is a standalone domain: never depends on CLI types.
 */

#include "wengine/logic/netlist.hpp"

#include <string_view>

namespace wengine::logic {

/**
 * @brief Remove a gate by display name (e.g. "AND1").
 *
 * Stays alias-monomorphic (text path): the scale harness and tests
 * remove by slot directly.
 *
 * @param[out] nl Netlist holding the gate.
 * @param[in] name Display name resolved via resolve_gate.
 * @return kOk on success; kInvalidArgument when the name does not resolve to a live gate.
 */
[[nodiscard]] inline core::Status remove_gate_by_name(Netlist &nl, std::string_view name) noexcept {
  auto gate = resolve_gate(nl, name);
  if (!gate.ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  return remove_gate(nl, gate.value);
}

/**
 * @brief Parse two pin spellings and wire output to input.
 *
 * Stays alias-monomorphic (text path): the scale harness and tests
 * wire by slot directly.
 *
 * @param[out] nl Netlist receiving the wire.
 * @param[in] from_text Output pin spelling (e.g. "AND2.Y").
 * @param[in] to_text Input pin spelling (e.g. "NOR1.1").
 * @return kOk on success; kInvalidArgument for unparsable pins or any
 *         wire_pins refusal.
 */
[[nodiscard]] inline core::Status wire_text(Netlist &nl, std::string_view from_text, std::string_view to_text) noexcept {
  auto from = parse_pin(nl, from_text);
  if (!from.ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  auto to = parse_pin(nl, to_text);
  if (!to.ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  return wire_pins(nl, from.value, to.value);
}

/**
 * @brief Parse two input-pin spellings and tie them (text adapter over
 *        the TieSet seam).
 *
 * Stays alias-monomorphic (text path).
 *
 * @param[out] nl Netlist receiving the tie.
 * @param[in] a_text First pin spelling.
 * @param[in] b_text Second pin spelling.
 * @return kOk on success; kInvalidArgument for unparsable pins or any
 *         tie_pins refusal.
 */
[[nodiscard]] inline core::Status tie_text(Netlist &nl, std::string_view a_text, std::string_view b_text) noexcept {
  auto a = parse_pin(nl, a_text);
  if (!a.ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  auto b = parse_pin(nl, b_text);
  if (!b.ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  return tie_pins(nl, a.value, b.value);
}

/**
 * @brief Parse one input-pin spelling and untie it.
 *
 * Stays alias-monomorphic (text path).
 *
 * @param[out] nl Netlist receiving the untie.
 * @param[in] text Pin spelling.
 * @return kOk on success; kInvalidArgument for unparsable pins or any
 *         untie_pin refusal.
 */
[[nodiscard]] inline core::Status untie_text(Netlist &nl, std::string_view text) noexcept {
  auto p = parse_pin(nl, text);
  if (!p.ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  return untie_pin(nl, p.value);
}

} // namespace wengine::logic
