#pragma once

// Frame-source selector panel: in-process sim (default, today's behavior),
// SHM subscriber attached to a headless publisher, or CSV replay playback
// of a CsvSink log. Renders ImGui UI only — all engine behavior lives in
// controllers/external_source.hpp (unit-tested, no ImGui linked).
//
// Wiring (see apps/gui/main.cpp): the panel owns UI text state (segment +
// path buffers, replay rate field); main owns the controllers, applies the
// segment text every tick, performs the CSV load on request (GUI owns
// file I/O), and selects the displayed frame by mode.
//
// Widget IDs: this panel shares the "WEngine" window scope with SimPanel,
// so every button label carries a ##suffix (ImGui IDs default to labels;
// bare duplicates across panels sharing a window misbehave).

#include "imgui.h"
#include "controllers/external_source.hpp"

#include <cstddef>
#include <cstdio>

namespace wengine::gui {

struct SourcePanel {
  int mode_idx = 0; // 0 in-process, 1 SHM subscribe, 2 CSV replay
  char segment[64] = "/wengine-demo";
  char csv_path[256] = "";
  double replay_rate = 1.0;
  bool load_requested = false; // set by the Load button, cleared by main

  [[nodiscard]] SourceMode mode() const noexcept {
    if (mode_idx == 1) {
      return SourceMode::kShm;
    }
    if (mode_idx == 2) {
      return SourceMode::kCsv;
    }
    return SourceMode::kInProcess;
  }

  void draw(ShmSource &shm, CsvReplay &replay) {
    ImGui::Separator();
    ImGui::Text("Frame source");
    ImGui::RadioButton("in-process", &mode_idx, 0);
    ImGui::SameLine();
    ImGui::RadioButton("SHM subscribe", &mode_idx, 1);
    ImGui::SameLine();
    ImGui::RadioButton("CSV replay", &mode_idx, 2);

    if (mode() == SourceMode::kShm) {
      ImGui::InputText("segment", segment, sizeof(segment));
      if (!shm.attached() && shm.ok()) {
        ImGui::TextDisabled("waiting for publisher '%s'...", segment);
      } else if (!shm.ok()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.4F, 0.4F, 1.0F));
        ImGui::TextWrapped("Error: %s", shm.error().c_str());
        ImGui::PopStyleColor();
      } else {
        const link::LagStats &lag = shm.lag();
        ImGui::TextDisabled("attached: %llu received, %llu missed (max gap %llu)",
                            static_cast<unsigned long long>(lag.received),
                            static_cast<unsigned long long>(lag.missed),
                            static_cast<unsigned long long>(lag.max_gap));
      }
      // Same-name publisher restarts need an explicit re-attach (POSIX fd
      // semantics: the old mapping outlives the rerun).
      if (ImGui::Button("Reconnect##shm")) {
        shm.reconnect();
      }
    } else if (mode() == SourceMode::kCsv) {
      ImGui::InputText("csv log", csv_path, sizeof(csv_path));
      ImGui::SameLine();
      if (ImGui::Button("Load")) {
        load_requested = true;
      }
      if (!replay.ok()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.4F, 0.4F, 1.0F));
        ImGui::TextWrapped("Error: %s", replay.error().c_str());
        ImGui::PopStyleColor();
      } else if (replay.count() == 0) {
        ImGui::TextDisabled("no log loaded.");
      } else {
        ImGui::TextDisabled("snapshot %llu / %llu", static_cast<unsigned long long>(replay.cursor() + 1),
                            static_cast<unsigned long long>(replay.count()));
        if (replay.playing()) {
          if (ImGui::Button("Pause##replay")) {
            replay.set_playing(false);
          }
        } else {
          if (ImGui::Button("Play##replay")) {
            replay.set_playing(true);
          }
        }
        ImGui::SameLine();
        if (ImGui::Button("Step##replay")) {
          replay.step_once();
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset##replay")) {
          replay.reset();
        }
        ImGui::InputDouble("rate", &replay_rate, 0.1, 1.0, "%.3f");
        replay.set_rate(replay_rate);
      }
    }
  }
};

} // namespace wengine::gui
