/* reasoning_panel.cc — ImGui rendering of ReasoningView. ImGui is a SYSTEM
 * include so Orthodoxy ignores its declarations; we only call its API. */
#include "solver/reasoning_panel.h"

#include <imgui.h>

#include "solver/engine.h" /* enum SolverEval */

static const char* verdict_text(int v) {
  switch (v) {
    case EVAL_START:
      return "START";
    case EVAL_SAFE:
      return "SAFE";
    case EVAL_GUESS:
      return "GUESS";
    case EVAL_SOLVED:
      return "SOLVED";
    case EVAL_LOST:
      return "LOST";
    default:
      return "?";
  }
}

static ImVec4 verdict_color(int v) {
  switch (v) {
    case EVAL_SAFE:
      return ImVec4(0.51f, 0.78f, 0.52f, 1.0f); /* green */
    case EVAL_GUESS:
      return ImVec4(1.0f, 0.72f, 0.30f, 1.0f); /* amber */
    case EVAL_LOST:
      return ImVec4(0.90f, 0.40f, 0.40f, 1.0f); /* red */
    default:
      return ImVec4(0.80f, 0.84f, 0.90f, 1.0f); /* slate */
  }
}

void reasoning_panel_draw(const struct ReasoningView* v) {
  /* Verdict */
  ImGui::TextColored(verdict_color(v->verdict), "%s", verdict_text(v->verdict));
  ImGui::SameLine();
  ImGui::TextDisabled(v->exact ? "[exact]" : "[approx]");

  if (v->verdict == EVAL_GUESS) {
    ImGui::TextDisabled("no proven-safe cell - must gamble");
  } else if (v->verdict == EVAL_SAFE) {
    ImGui::TextDisabled("a proven move exists - no guess");
  }

  ImGui::Separator();

  /* Recommendation */
  if (v->has_move) {
    ImGui::Text("Move (%d, %d)", v->move_x, v->move_y);
    ImGui::Text("risk %d%% mine", v->risk_pct);
    ImGui::ProgressBar((float)v->risk_pct / 100.0f, ImVec2(-1.0f, 6.0f), "");
    if (v->verdict == EVAL_SAFE) {
      ImGui::BulletText("forced safe by deduction");
    } else if (v->verdict == EVAL_START) {
      ImGui::BulletText("first move - always safe");
    } else if (v->verdict == EVAL_GUESS) {
      if (v->took_riskier) {
        ImGui::BulletText("took +%d%% over safest (%d%%)",
                          v->risk_pct - v->safest_pct, v->safest_pct);
      } else {
        ImGui::BulletText("lowest-risk cell");
      }
      if (v->pick_gain > 0) {
        ImGui::BulletText("unlocks %d more cell%s if safe", v->pick_gain,
                          v->pick_gain == 1 ? "" : "s");
      }
    }
  } else {
    ImGui::TextDisabled("(no move)");
  }

  ImGui::Separator();

  /* This turn */
  ImGui::Text("proven safe   %d", v->proven_safe);
  ImGui::Text("proven mine   %d", v->proven_mine);
  ImGui::Text("frontier      %d", v->frontier);
  if (v->interior_count > 0) {
    ImGui::Text("interior      %d%% x %d", v->interior_pct, v->interior_count);
  }
  ImGui::Text("mines total   %d", v->mines_total);

  ImGui::Separator();

  /* Hover inspect */
  if (v->hover_valid) {
    ImGui::Text("cell (%d, %d): %d%% mine", v->hover_x, v->hover_y,
                v->hover_pct);
    if (v->hover_forced_safe) {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(0.51f, 0.78f, 0.52f, 1.0f), "(safe)");
    } else if (v->hover_forced_mine) {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(0.90f, 0.40f, 0.40f, 1.0f), "(mine)");
    }
    if (v->hover_frontier && v->hover_gain > 0) {
      ImGui::Text("  unlocks %d if safe", v->hover_gain);
    }
  } else {
    ImGui::TextDisabled("hover a covered cell to inspect");
  }
}
