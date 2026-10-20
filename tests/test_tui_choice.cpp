// TUI enum-choice dialogs: static + registry-derived options and the
// shared ncurses-free dialog model.
//
// New commands/arguments get the Space-opened dialog by declaring `choices`
// on the field (static vocabularies) or via the resolvers (dynamic ones) —
// no pane logic changes. This pins that contract: every current enum field
// resolves, free-text fields stay empty, and a synthetic future field with
// fresh choices resolves with no code change.

#include "wengine/tui/choice.hpp"
#include "wengine/tui/forms.hpp"
#include "wengine/tui/batches.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace wengine;

namespace {

int form_field(const tui::forms::FormDef &def, std::string_view key_or_label) {
  for (std::size_t i = 0; i < def.fields.size(); ++i) {
    if (def.fields[i].key == key_or_label || def.fields[i].label == key_or_label) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int batch_field(const tui::batches::BatchDef &def, std::string_view key) {
  for (std::size_t i = 0; i < def.fields.size(); ++i) {
    if (def.fields[i].key == key) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

const tui::forms::FormDef *find_form(const std::vector<tui::forms::FormDef> &defs,
                                     std::string_view cmd) {
  for (const auto &d : defs) {
    if (d.command == cmd) {
      return &d;
    }
  }
  return nullptr;
}

const tui::batches::BatchDef *find_batch(const std::vector<tui::batches::BatchDef> &defs,
                                         std::string_view cmd) {
  for (const auto &d : defs) {
    if (d.command == cmd) {
      return &d;
    }
  }
  return nullptr;
}

bool contains(const std::vector<std::string> &v, std::string_view s) {
  for (const auto &e : v) {
    if (e == s) {
      return true;
    }
  }
  return false;
}

void test_dialog_model() {
  tui::ChoiceDialog d;
  assert(!d.open);
  // Empty options never open.
  assert(!tui::open_choice(d, "t", {}, 0, ""));
  assert(!d.open);
  // Current value tracks the highlight.
  assert(tui::open_choice(d, "mode", {"a", "b", "c"}, 2, "b"));
  assert(d.open && d.selected == 1 && d.field == 2);
  tui::choice_move(d, 1);
  assert(d.selected == 2);
  tui::choice_move(d, 1); // clamped at the end
  assert(d.selected == 2);
  tui::choice_move(d, -1);
  assert(d.selected == 1);
  assert(tui::choice_confirmed(d) == "b");
  // Unknown current parks on the first option.
  assert(tui::open_choice(d, "t", {"x", "y"}, 0, "zzz"));
  assert(d.selected == 0 && tui::choice_confirmed(d) == "x");
  tui::close_choice(d);
  assert(!d.open && d.options.empty());
  assert(tui::choice_confirmed(d).empty());
}

void test_form_static_choices() {
  const auto defs = tui::forms::form_defs();
  const auto *ohm = find_form(defs, "ohm");
  const auto *s0 = find_form(defs, "s0");
  const auto *i2c = find_form(defs, "i2c");
  const auto *ldo = find_form(defs, "ldo");
  const auto *ic = find_form(defs, "ic");
  assert(ohm && s0 && i2c && ldo && ic);

  {
    auto v = std::vector<std::string>(ohm->fields.size());
    const auto opts = tui::forms::form_field_choices(
        *ohm, static_cast<std::size_t>(form_field(*ohm, "--from")), v);
    assert(opts.size() == 6);
    for (std::string_view want : {"ui", "ur", "ir", "iu", "ru", "ri"}) {
      assert(contains(opts, want));
    }
    // Free-text physics fields stay dialogs-free.
    assert(tui::forms::form_field_choices(
               *ohm, static_cast<std::size_t>(form_field(*ohm, "--u")), v)
               .empty());
  }
  {
    auto v = std::vector<std::string>(s0->fields.size());
    const auto opts = tui::forms::form_field_choices(
        *s0, static_cast<std::size_t>(form_field(*s0, "mode")), v);
    assert(opts.size() == 6 && contains(opts, "rate") && contains(opts, "full"));
    assert(tui::forms::form_field_choices(
               *s0, static_cast<std::size_t>(form_field(*s0, "--power")), v)
               .empty());
  }
  {
    auto v = std::vector<std::string>(i2c->fields.size());
    const auto mode = tui::forms::form_field_choices(
        *i2c, static_cast<std::size_t>(form_field(*i2c, "mode")), v);
    assert(mode.size() == 3 && contains(mode, "pullup") && contains(mode, "full"));
    const auto speed = tui::forms::form_field_choices(
        *i2c, static_cast<std::size_t>(form_field(*i2c, "--speed")), v);
    assert(speed.size() == 3 && contains(speed, "std") && contains(speed, "fmp"));
    assert(tui::forms::form_field_choices(
               *i2c, static_cast<std::size_t>(form_field(*i2c, "--vdd")), v)
               .empty());
  }
  {
    auto v = std::vector<std::string>(ldo->fields.size());
    assert(tui::forms::form_field_choices(
               *ldo, static_cast<std::size_t>(form_field(*ldo, "--vin")), v)
               .empty());
  }
  {
    auto v = std::vector<std::string>(ic->fields.size());
    const auto bits = tui::forms::form_field_choices(
        *ic, static_cast<std::size_t>(form_field(*ic, "--bits")), v);
    assert(bits.size() == 4 && contains(bits, "9") && contains(bits, "12"));
    const auto avg = tui::forms::form_field_choices(
        *ic, static_cast<std::size_t>(form_field(*ic, "--avg")), v);
    assert(avg.size() == 8 && contains(avg, "1") && contains(avg, "128"));
    const auto brng = tui::forms::form_field_choices(
        *ic, static_cast<std::size_t>(form_field(*ic, "--brng")), v);
    assert(brng.size() == 2 && contains(brng, "16") && contains(brng, "32"));
    const auto pga = tui::forms::form_field_choices(
        *ic, static_cast<std::size_t>(form_field(*ic, "--pga")), v);
    assert(pga.size() == 4 && contains(pga, "40") && contains(pga, "320"));
    const auto mode = tui::forms::form_field_choices(
        *ic, static_cast<std::size_t>(form_field(*ic, "--mode")), v);
    assert(contains(mode, "powerdown") && contains(mode, "both") && contains(mode, "7"));
    // Free-text chip options stay dialogs-free.
    assert(tui::forms::form_field_choices(
               *ic, static_cast<std::size_t>(form_field(*ic, "--vin")), v)
               .empty());
  }
}

void test_form_dynamic_choices() {
  const auto defs = tui::forms::form_defs();
  const auto *ic = find_form(defs, "ic");
  assert(ic != nullptr);
  auto v = std::vector<std::string>(ic->fields.size());
  const int chip = form_field(*ic, "chip");
  const int op = form_field(*ic, "op");
  assert(chip >= 0 && op >= 0);
  // Chips come from IcRegistry, never hardcoded.
  const auto chips = tui::forms::form_field_choices(*ic, static_cast<std::size_t>(chip), v);
  assert(!chips.empty() && contains(chips, "ap63205") && contains(chips, "ina219"));
  // Ops follow the chip value.
  v[static_cast<std::size_t>(chip)] = "ap63205";
  const auto ap_ops =
      tui::forms::form_field_choices(*ic, static_cast<std::size_t>(op), v);
  assert(contains(ap_ops, "inductor") && contains(ap_ops, "spec"));
  v[static_cast<std::size_t>(chip)] = "ina219";
  const auto ina_ops =
      tui::forms::form_field_choices(*ic, static_cast<std::size_t>(op), v);
  assert(contains(ina_ops, "cal") && contains(ina_ops, "config"));
  // Unknown/empty chip offers nothing (no misleading dialog).
  v[static_cast<std::size_t>(chip)] = "nope-chip";
  assert(tui::forms::form_field_choices(*ic, static_cast<std::size_t>(op), v).empty());
  v[static_cast<std::size_t>(chip)].clear();
  assert(tui::forms::form_field_choices(*ic, static_cast<std::size_t>(op), v).empty());
  // Out-of-range fields never open.
  assert(tui::forms::form_field_choices(*ic, ic->fields.size() + 4, v).empty());
}

void test_batch_choices() {
  const auto defs = tui::batches::batch_defs();
  const auto *sim = find_batch(defs, "sim");
  const auto *sig = find_batch(defs, "sig");
  assert(sim && sig);

  {
    auto v = std::vector<std::string>(sim->fields.size());
    const auto solvers = tui::batches::batch_field_choices(
        *sim, static_cast<std::size_t>(batch_field(*sim, "--solver")), v);
    assert(!solvers.empty() && contains(solvers, "projectile") && contains(solvers, "signal"));
    const auto uart = tui::batches::batch_field_choices(
        *sim, static_cast<std::size_t>(batch_field(*sim, "--emit-uart-mode")), v);
    assert(uart.size() == 2 && contains(uart, "ascii") && contains(uart, "cobs"));
    // Numeric/path fields stay dialogs-free.
    assert(tui::batches::batch_field_choices(
               *sim, static_cast<std::size_t>(batch_field(*sim, "--steps")), v)
               .empty());
  }
  {
    auto v = std::vector<std::string>(sig->fields.size());
    const auto kinds = tui::batches::batch_field_choices(
        *sig, static_cast<std::size_t>(batch_field(*sig, "--sig-kind")), v);
    assert(!kinds.empty() && contains(kinds, "sine") && contains(kinds, "lua"));
    const auto qty = tui::batches::batch_field_choices(
        *sig, static_cast<std::size_t>(batch_field(*sig, "--sig-quantity")), v);
    assert(qty.size() == 2 && contains(qty, "U") && contains(qty, "I"));
    const auto uart = tui::batches::batch_field_choices(
        *sig, static_cast<std::size_t>(batch_field(*sig, "--emit-uart-mode")), v);
    assert(uart.size() == 2 && contains(uart, "ascii") && contains(uart, "cobs"));
    assert(tui::batches::batch_field_choices(
               *sig, static_cast<std::size_t>(batch_field(*sig, "--sig-freq")), v)
               .empty());
    assert(tui::batches::batch_field_choices(*sig, sig->fields.size() + 1, v).empty());
  }
}

void test_future_field_scales() {
  // A future command declares its enum inline; the resolver picks it up
  // with no pane/model change.
  tui::forms::FormDef future{};
  future.command = "next";
  future.fields = {{"--mode", "mode", "pick one", {"alpha", "beta", "gamma"}},
                   {"--value", "value", "free text"}};
  const auto vals = std::vector<std::string>(2);
  const auto opts = tui::forms::form_field_choices(future, 0, vals);
  assert(opts.size() == 3 && contains(opts, "beta"));
  assert(tui::forms::form_field_choices(future, 1, vals).empty());

  tui::batches::BatchDef future_batch{};
  future_batch.command = "next";
  future_batch.mode = "run";
  future_batch.fields = {{"--level", "level", "pick one", false, {"low", "high"}},
                         {"--path", "path", "free text"}};
  const auto bvals = std::vector<std::string>(2);
  const auto bopts = tui::batches::batch_field_choices(future_batch, 0, bvals);
  assert(bopts.size() == 2 && contains(bopts, "high"));
  assert(tui::batches::batch_field_choices(future_batch, 1, bvals).empty());

  // The dialog highlights the stored value when reopened.
  tui::ChoiceDialog d;
  assert(tui::open_choice(d, "level", bopts, 0, "high"));
  assert(d.selected == 1 && tui::choice_confirmed(d) == "high");
}

} // namespace

int main() {
  test_dialog_model();
  test_form_static_choices();
  test_form_dynamic_choices();
  test_batch_choices();
  test_future_field_scales();
  std::cout << "test_tui_choice: OK\n";
  return 0;
}
