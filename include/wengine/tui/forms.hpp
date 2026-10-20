#pragma once

// F3 command-form registry. Form data is defined one command per file under
// forms/; register a new form here and keep the registry order stable.

#include "wengine/tui/forms/types.hpp"

#include "wengine/tui/forms/ohm.hpp"
#include "wengine/tui/forms/s0.hpp"
#include "wengine/tui/forms/i2c.hpp"
#include "wengine/tui/forms/ldo.hpp"
#include "wengine/tui/forms/led.hpp"
#include "wengine/tui/forms/ic.hpp"

#include <array>
#include <vector>

namespace wengine::tui::forms {

using FormFactory = FormDef (*)();

// Single registration point, in the stable order shown by the F3 browser.
inline constexpr std::array<FormFactory, kFormCount> kFormRegistry{{
    &definitions::ohm,
    &definitions::s0,
    &definitions::i2c,
    &definitions::ldo,
    &definitions::led,
    &definitions::ic,
}};

[[nodiscard]] inline std::vector<FormDef> form_defs() {
  std::vector<FormDef> defs;
  defs.reserve(kFormRegistry.size());
  for (const FormFactory make_form : kFormRegistry) {
    defs.push_back(make_form());
  }
  return defs;
}

} // namespace wengine::tui::forms
