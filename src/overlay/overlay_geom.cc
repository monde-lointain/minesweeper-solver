/* overlay_geom.cc — pure overlay helpers (Stream C). No SDL/ImGui.
 *
 * Orthodox C++: POD by value, C headers, C-style casts, index loops.
 */
#include "solver/overlay_geom.h"

#include <math.h>
#include <stdio.h>

#include "solver/util.h"

struct OverlayRect overlay_cell_rect(int grid_x, int grid_y, int cell, int x,
                                     int y) {
  struct OverlayRect r;
  r.x = grid_x + (x * cell);
  r.y = grid_y + (y * cell);
  r.w = cell;
  r.h = cell;
  return r;
}

/* Linear interpolate two 0..255 channel endpoints, rounded and clamped. */
static unsigned char lerp8(int a, int b, double t) {
  double v = (double)a + (((double)b - (double)a) * t);
  return (unsigned char)solver_clampi((int)lround(v), 0, 255);
}

struct OverlayColor overlay_prob_color(double prob) {
  double p = (prob < 0.0) ? 0.0 : ((prob > 1.0) ? 1.0 : prob);
  struct OverlayColor c;
  /* 0% green (60,200,60) -> 50% yellow (240,200,40) -> 100% red (225,70,70) */
  if (p < 0.5) {
    double t = p / 0.5;
    c.r = lerp8(60, 240, t);
    c.g = lerp8(200, 200, t);
    c.b = lerp8(60, 40, t);
  } else {
    double t = (p - 0.5) / 0.5;
    c.r = lerp8(240, 225, t);
    c.g = lerp8(200, 70, t);
    c.b = lerp8(40, 40, t);
  }
  c.a = 148; /* ~0.58 alpha over the gray tile */
  return c;
}

static int safety_pct(double mine_prob) {
  double s = (1.0 - mine_prob) * 100.0;
  return solver_clampi((int)lround(s), 0, 100);
}

void overlay_eval_string(const struct Analysis* a, char* buf, int n) {
  if (n <= 0) {
    return;
  }
  buf[0] = '\0';
  switch (a->eval) {
    case EVAL_SAFE:
      snprintf(buf, (size_t)n, "SAFE  best (%d,%d)  %d%% safe", a->best_x,
               a->best_y, safety_pct(a->best_prob));
      break;
    case EVAL_GUESS:
      if (a->interior_count > 0) {
        snprintf(buf, (size_t)n,
                 "GUESS  best (%d,%d)  %d%% safe  | interior %d%%", a->best_x,
                 a->best_y, safety_pct(a->best_prob),
                 safety_pct(a->interior_prob));
      } else {
        snprintf(buf, (size_t)n, "GUESS  best (%d,%d)  %d%% safe", a->best_x,
                 a->best_y, safety_pct(a->best_prob));
      }
      break;
    case EVAL_START:
      snprintf(buf, (size_t)n, "START  open (%d,%d)", a->best_x, a->best_y);
      break;
    case EVAL_SOLVED:
      snprintf(buf, (size_t)n, "SOLVED");
      break;
    case EVAL_LOST:
      snprintf(buf, (size_t)n, "LOST");
      break;
    default:
      break;
  }
}
