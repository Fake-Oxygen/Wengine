// TUI-6 output views: highlight, search, save, resize reflow, no-color
// fallback, and scroll position — all on captured CLI text with no
// per-command view code (D-TUI-1 holds: the model never parses commands).

#include "wengine/tui/views.hpp"
#include "wengine/tui/theme.hpp"
#include "wengine/tui/theme_config.hpp"
#include "wengine/cli/shell.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace wengine;

namespace {

std::string tmp(const char *name);

void test_classify() {
  using K = tui::views::LineKind;
  using tui::views::classify_line;
  assert(classify_line("> ohm 12V 3A") == K::kPrompt);
  assert(classify_line("[... 3 lines truncated -- rerun with --emit-csv for the full log]") == K::kTruncation);
  assert(classify_line("summary: PASS") == K::kPass);
  assert(classify_line("summary: FAIL") == K::kFail);
  assert(classify_line("P rating = 0.06 W OK") == K::kPass);
  assert(classify_line("tr @Rp+Rs = 100 ns FAIL") == K::kFail);
  assert(classify_line("Error: unknown signal kind (want sine|square|triangle|saw|dc|") == K::kError);
  assert(classify_line("Unknown command: nopecmd") == K::kError);
  assert(classify_line("WARN: above the 26V absolute maximum (Sec. 7.1) FAIL") == K::kFail ||
         classify_line("WARN: above the 26V absolute maximum (Sec. 7.1) FAIL") == K::kError);
  // Usage doc mentioning PASS/FAIL together is not a verdict.
  assert(classify_line("  --tjmax <C> (thermal limit for PASS/FAIL;") == K::kNormal);
  assert(classify_line("plain output line") == K::kNormal);
  assert(classify_line("") == K::kNormal);
  // Pair mapping stays stable for the draw layer.
  assert(tui::views::color_pair_for(K::kPass) == tui::views::kPassPair);
  assert(tui::views::color_pair_for(K::kFail) == tui::views::kFailPair);
  assert(tui::views::color_pair_for(K::kError) == tui::views::kFailPair);
  assert(tui::views::color_pair_for(K::kPrompt) == tui::views::kPromptPair);
  assert(tui::views::color_pair_for(K::kNormal) == 0);
}

void test_colors_allowed() {
  using tui::views::colors_allowed;
  assert(colors_allowed("xterm", false));
  assert(colors_allowed("xterm-256color", false));
  assert(!colors_allowed("dumb", false));
  assert(!colors_allowed("", false));
  assert(!colors_allowed("unknown", false));
  assert(!colors_allowed("xterm", true));
  assert(!colors_allowed("dumb", true));
  // Full-screen gate: dumb/unknown/empty refuse loudly; everything else
  // (including NO_COLOR xterms) still runs the panes.
  assert(tui::views::full_screen_supported("xterm"));
  assert(tui::views::full_screen_supported("xterm-256color"));
  assert(!tui::views::full_screen_supported("dumb"));
  assert(!tui::views::full_screen_supported("unknown"));
  assert(!tui::views::full_screen_supported(""));
  // Runtime env gate defaults to enabled here (CI TERM is not dumb and
  // NO_COLOR is unset); the PTY script pins both branches explicitly.
  (void)tui::views::colors_allowed_env();
}

void test_theme_selection() {
  using namespace tui::themes;
  assert(kThemes.size() == 7);
  assert(find("terminal").name == "terminal");
  assert(find("default").name == "terminal");
  assert(find("catppuccin-mocha").name == "catppuccin-mocha");
  assert(find("dracula").name == "dracula");
  assert(find("nord").name == "nord");
  assert(find("gruvbox-dark").name == "gruvbox-dark");
  assert(find("tokyo-night").name == "tokyo-night");
  assert(find("light").name == "light");
  assert(find("not-a-theme").name == "terminal");
  assert(index_of("dracula") == 2);
  assert(contains("terminal") && contains("default") && contains("catppuccin-mocha"));
  assert(!contains("not-a-theme"));
  // Every palette uses the xterm 256-color range (or the terminal defaults).
  for (const Theme &theme : kThemes) {
    const PairStyle styles[]{theme.background, theme.title, theme.success_status, theme.error_status,
                             theme.pass, theme.fail, theme.prompt};
    for (const PairStyle style : styles) {
      assert(style.foreground >= kDefaultColor && style.foreground <= 255);
      assert(style.background >= kDefaultColor && style.background <= 255);
    }
  }
}

void test_theme_preference_round_trip() {
  const std::string path = tmp("wengine_test_tui_theme_preference");
  const std::string temporary = path + ".tmp";
  std::remove(path.c_str());
  std::remove(temporary.c_str());
  std::string error;
  assert(tui::themes::save_preference(path, "dracula", error));
  assert(tui::themes::load_preference(path) == "dracula");
  assert(tui::themes::save_preference(path, "catppuccin-mocha", error));
  assert(tui::themes::load_preference(path) == "catppuccin-mocha");
  assert(!tui::themes::save_preference(path, "not-a-theme", error));
  assert(!error.empty());
  std::remove(path.c_str());
  std::remove(temporary.c_str());
}

void test_find_matches() {
  using tui::views::find_matches;
  const std::vector<std::string> lines{"ohm result", "summary: PASS", "Error: bad", "summary: PASS again"};
  {
    const auto m = find_matches(lines, "PASS");
    assert(m.size() == 2 && m[0] == 1 && m[1] == 3);
  }
  assert(find_matches(lines, "").empty());
  assert(find_matches(lines, "zzz").empty());
  // Case-sensitive: lowercase does not hit the verdict token.
  assert(find_matches(lines, "pass").empty());
  // Search spans the whole logical scrollback, not just the visible page:
  // a 600-command tail (500-line cap policy leaves a truncation note) is
  // still fully searchable.
  std::vector<std::string> big;
  for (int i = 0; i < 600; ++i) {
    big.push_back("line " + std::to_string(i) + (i == 0 || i == 599 ? " NEEDLE" : ""));
  }
  big.push_back("[... 100 lines truncated -- rerun with --emit-csv for the full log]");
  const auto m = find_matches(big, "NEEDLE");
  assert(m.size() == 2 && m[0] == 0 && m[1] == 599);
  // The truncation note itself is searchable text like any other line.
  assert(find_matches(big, "truncated").size() == 1);
}

void test_match_spans() {
  using tui::views::match_spans;
  {
    const auto sp = match_spans("aaa", "aa");
    assert(sp.size() == 1 && sp[0].first == 0 && sp[0].second == 2);
  }
  {
    const auto sp = match_spans("one two one", "one");
    assert(sp.size() == 2 && sp[0].first == 0 && sp[1].first == 8);
  }
  assert(match_spans("abc", "").empty());
  assert(match_spans("abc", "zzz").empty());
}

void test_wrap_reflow() {
  using tui::views::wrap_lines;
  const std::vector<std::string> logical{"abcdefghij", "", "x"};
  {
    const auto w = wrap_lines(logical, 4);
    assert(w.rows.size() == 5);  // 3 + 1 + 1
    assert(w.rows[0] == "abcd" && w.rows[1] == "efgh" && w.rows[2] == "ij");
    assert(w.rows[3].empty() && w.rows[4] == "x");
    assert(w.logical_for_row[0] == 0 && w.logical_for_row[2] == 0);
    assert(w.logical_for_row[3] == 1 && w.logical_for_row[4] == 2);
    assert(w.row_start_for_logical[0] == 0 && w.row_start_for_logical[1] == 3);
  }
  // Narrowing grows the display rows (reflow); widening shrinks them.
  {
    const auto narrow = wrap_lines(logical, 3);
    const auto wide = wrap_lines(logical, 20);
    assert(narrow.rows.size() > wide.rows.size());
    assert(wide.rows.size() == 3);
  }
  // Degenerate widths never corrupt: clamped to 1, ASCII-exact.
  {
    const auto w = wrap_lines(logical, 0);
    assert(!w.rows.empty() && w.rows[0].size() == 1);
  }
}

void test_scroll_math() {
  using tui::views::clamp_scroll;
  using tui::views::display_first;
  using tui::views::scroll_to_show;
  // Follow mode: no scroll room, first is always 0.
  assert(clamp_scroll(5, 3, 10) == 0);
  assert(display_first(3, 10, 0) == 0);
  // 120 display rows on a 30-row page: scroll range is 0..90.
  assert(clamp_scroll(-3, 120, 30) == 0);
  assert(clamp_scroll(999, 120, 30) == 90);
  assert(display_first(120, 30, 0) == 90);   // tail
  assert(display_first(120, 30, 90) == 0);   // top
  assert(display_first(120, 30, 45) == 45);  // middle
  // Jump parks the hit at the top when there is room above.
  assert(scroll_to_show(5, 120, 30) == 85);
  assert(scroll_to_show(95, 120, 30) == 0);  // near tail clamps to follow
  assert(scroll_to_show(0, 5, 30) == 0);     // fits: no scroll
  // Resize reflow never strands the viewport: clamping a stale large
  // scroll after widening (fewer display rows) lands back in range.
  const std::vector<std::string> logical(50, std::string(40, 'a'));
  const auto narrow = tui::views::wrap_lines(logical, 10);  // 50*4 = 200 rows
  const auto wide = tui::views::wrap_lines(logical, 40);    // 50 rows
  assert(narrow.rows.size() == 200 && wide.rows.size() == 50);
  const int stale = 150;
  assert(clamp_scroll(stale, wide.rows.size(), 20) == 30);
  assert(display_first(wide.rows.size(), 20, 30) + 20 <= wide.rows.size());
}

void test_scroll_indicator() {
  using tui::views::scroll_indicator;
  assert(scroll_indicator(0, 0, 0) == "0/0");
  assert(scroll_indicator(0, 30, 30) == "1-30/30 (100%)");
  assert(scroll_indicator(90, 30, 120) == "91-120/120 (100%)");
  assert(scroll_indicator(0, 30, 120) == "1-30/120 (25%)");
}

std::string tmp(const char *name) { return std::string("/tmp/opencode/") + name; }

void test_save_reproduces_text() {
  const std::vector<std::string> lines{"> sim run --steps 2", "summary: PASS", "Error: demo"};
  const std::string path = tmp("wengine_test_tui_views_save.txt");
  std::remove(path.c_str());
  std::string err;
  assert(tui::views::save_lines(lines, path, err));
  std::ifstream in(path);
  assert(static_cast<bool>(in));
  std::vector<std::string> back;
  std::string l;
  while (std::getline(in, l)) {
    back.push_back(l);
  }
  assert(back == lines);
  std::remove(path.c_str());
  // Loud failure, never a throw/silent drop.
  assert(!tui::views::save_lines(lines, "/nonexistent-dir-xyz/file.txt", err));
  assert(!err.empty());
}

void test_no_per_command_views() {
  // Rendering stays on captured text for every solver family: run one line
  // per domain through the real Shell and prove the generic classifier
  // finds each verdict without any command-specific branch.
  cli::Shell shell;
  shell.set_history_file("");
  const char *lines[] = {
      "led --vs 5V --vf 2V --if 20mA",
      "ldo --vin 5V --vout 3.3V --iout 100mA",
      "i2c full --vdd 3.3V --cb 200pF --rp 1.5kOhm --rs 100Ohm",
      "ohm 12V 3A",
      "nopecmd",
  };
  bool saw_pass = false;
  bool saw_error = false;
  for (const char *c : lines) {
    std::ostringstream out;
    (void)shell.execute(c, out);
    std::istringstream text(out.str());
    std::string line;
    while (std::getline(text, line)) {
      const auto k = tui::views::classify_line(line);
      if (k == tui::views::LineKind::kPass) {
        saw_pass = true;
      }
      if (k == tui::views::LineKind::kError) {
        saw_error = true;
      }
      // No throw, no command check: the verdict is in the text alone.
      (void)tui::views::color_pair_for(k);
    }
  }
  assert(saw_pass && saw_error);
}

}  // namespace

int main() {
  test_classify();
  test_colors_allowed();
  test_theme_selection();
  test_theme_preference_round_trip();
  test_find_matches();
  test_match_spans();
  test_wrap_reflow();
  test_scroll_math();
  test_scroll_indicator();
  test_save_reproduces_text();
  test_no_per_command_views();
  std::cout << "test_tui_views: OK\n";
  return 0;
}
