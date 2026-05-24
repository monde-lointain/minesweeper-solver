/* overlay.cc — ImGui analysis overlay (Stream E).
 *
 * Drawn on the ImGui foreground draw list between render_frame and
 * ImGui::Render: probability tints + % on covered frontier cells, a blue inset
 * ring on the best move, and a click-through floating eval readout. Interior
 * cells are left plain; flagged/? cells keep their original sprite. No-op when
 * the game is over.
 *
 * ImGui boundary: <imgui.h> is a SYSTEM include so Orthodoxy ignores its
 * declarations; we only call its API (no C++ features declared in our code).
 */
#include "solver/overlay.h"

#include <math.h>
#include <stdio.h>

#include <imgui.h>

#include "solver/overlay_geom.h"

void overlay_draw(const struct Analysis *a, const struct Board *b,
                  const struct Layout *lay) {
  if (b->status == GAME_WON || b->status == GAME_LOST) {
    return;
  }

  ImDrawList *dl = ImGui::GetForegroundDrawList();
  int cell = BLOCK_PX * lay->scale;
  ImU32 text_col = IM_COL32(20, 20, 20, 255);

  /* frontier tints + % */
  for (int y = 0; y < b->height; ++y) {
    for (int x = 0; x < b->width; ++x) {
      int i = game_index(b, x, y);
      const struct Cell *c = &b->cells[i];
      if (c->revealed || c->flag != FLAG_NONE) {
        continue;
      }
      const struct CellAnalysis *ca = &a->cells[i];
      if (!ca->is_frontier) {
        continue; /* interior cells stay plain */
      }
      struct OverlayRect r = overlay_cell_rect(lay->grid_x, lay->grid_y, cell, x,
                                               y);
      struct OverlayColor col = overlay_prob_color(ca->mine_prob);
      ImVec2 p0((float)r.x, (float)r.y);
      ImVec2 p1((float)(r.x + r.w), (float)(r.y + r.h));
      dl->AddRectFilled(p0, p1, IM_COL32(col.r, col.g, col.b, col.a));

      if (lay->scale >= 2) {
        char buf[8];
        long pct = lround(ca->mine_prob * 100.0);
        pct = (pct < 0) ? 0 : ((pct > 100) ? 100 : pct);
        snprintf(buf, sizeof buf, "%ld", pct);
        ImVec2 ts = ImGui::CalcTextSize(buf);
        float tx = (float)r.x + (((float)r.w - ts.x) * 0.5f);
        float ty = (float)r.y + (((float)r.h - ts.y) * 0.5f);
        dl->AddText(ImVec2(tx, ty), text_col, buf);
      }
    }
  }

  /* best-move blue inset ring (may land on an un-tinted interior cell) */
  if (a->best_x >= 0 && a->best_y >= 0) {
    int bi = game_index(b, a->best_x, a->best_y);
    if (!b->cells[bi].revealed) {
      struct OverlayRect r =
          overlay_cell_rect(lay->grid_x, lay->grid_y, cell, a->best_x,
                            a->best_y);
      ImVec2 p0((float)(r.x + 1), (float)(r.y + 1));
      ImVec2 p1((float)((r.x + r.w) - 1), (float)((r.y + r.h) - 1));
      float thickness = (float)lay->scale + 1.0f;
      dl->AddRect(p0, p1, IM_COL32(21, 101, 192, 255), 0.0f, 0, thickness);
    }
  }

  /* click-through floating eval readout (bottom-left) */
  char eb[160];
  overlay_eval_string(a, eb, (int)sizeof eb);
  if (eb[0] != '\0') {
    ImVec2 ts = ImGui::CalcTextSize(eb);
    float pad = 4.0f;
    float bx = 4.0f;
    float by = (float)lay->window_h - (ts.y + (2.0f * pad)) - 4.0f;
    ImVec2 q0(bx, by);
    ImVec2 q1(bx + ts.x + (2.0f * pad), by + ts.y + (2.0f * pad));
    dl->AddRectFilled(q0, q1, IM_COL32(0, 0, 0, 190), 4.0f);
    dl->AddText(ImVec2(bx + pad, by + pad), IM_COL32(255, 255, 255, 255), eb);
  }
}
