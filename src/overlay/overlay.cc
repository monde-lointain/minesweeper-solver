/* overlay.cc — ImGui analysis overlay (Stream E).
 *
 * Drawn on the ImGui background draw list between render_frame and
 * ImGui::Render: a single blue inset box around the recommended move — the
 * exact cell the win-rate policy plays (solver_recommend_move, shared with the
 * benchmark). No-op when the game is over or no covered cell remains.
 *
 * ImGui boundary: <imgui.h> is a SYSTEM include so Orthodoxy ignores its
 * declarations; we only call its API (no C++ features declared in our code).
 */
#include "solver/overlay.h"

#include <imgui.h>

#include "solver/overlay_geom.h"
#include "solver/recommend.h"

void overlay_draw(const struct Analysis* a, const struct Board* b,
                  const struct Layout* lay) {
  if (b->status == GAME_WON || b->status == GAME_LOST) {
    return;
  }

  /* The recommended move = the move the win-rate policy would play. */
  int rx = 0;
  int ry = 0;
  if (solver_recommend_move(b, a, &rx, &ry) != 0) {
    return; /* no covered cell to recommend */
  }
  if (b->cells[game_index(b, rx, ry)].revealed) {
    return;
  }

  /* Background draw list: over the SDL board but UNDER ImGui windows/menus, so
   * open dropdowns/dialogs correctly occlude the overlay. */
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  int cell = BLOCK_PX * lay->scale;
  struct OverlayRect r =
      overlay_cell_rect(lay->grid_x, lay->grid_y, cell, rx, ry);
  ImVec2 p0((float)(r.x + 1), (float)(r.y + 1));
  ImVec2 p1((float)((r.x + r.w) - 1), (float)((r.y + r.h) - 1));
  float thickness = (float)lay->scale + 1.0f;
  dl->AddRect(p0, p1, IM_COL32(21, 101, 192, 255), 0.0f, 0, thickness);
}
