/* reasoning.cc — pure Analysis -> ReasoningView extraction.
 * No SDL/ImGui. Orthodox C++: POD, pointers, C headers, index loops. */
#include "solver/reasoning.h"

#include <math.h>
#include <string.h>

#include "solver/recommend.h"
#include "solver/util.h"

static int reasoning_pct(double p) {
  return solver_clampi((int)lround(p * 100.0), 0, 100);
}

void reasoning_build(const struct Board* b, const struct Analysis* a,
                     int hover_x, int hover_y, struct ReasoningView* out) {
  memset(out, 0, sizeof *out);
  out->verdict = a->eval;
  out->exact = a->exact;
  out->interior_count = a->interior_count;
  out->interior_pct = reasoning_pct(a->interior_prob);
  out->mines_total = b->mines;

  int n = b->width * b->height;
  for (int i = 0; i < n; ++i) {
    if (b->cells[i].revealed) {
      continue;
    }
    if (a->cells[i].forced_safe) {
      ++out->proven_safe;
    }
    if (a->cells[i].forced_mine) {
      ++out->proven_mine;
    }
    if (a->cells[i].is_frontier) {
      ++out->frontier;
    }
  }

  int rx = -1;
  int ry = -1;
  if (solver_recommend_move(b, a, &rx, &ry) == 0) {
    int idx = game_index(b, rx, ry);
    out->has_move = true;
    out->move_x = rx;
    out->move_y = ry;
    out->risk_pct = reasoning_pct(a->cells[idx].mine_prob);
    out->pick_gain = a->cells[idx].info_gain;

    double pmin = 2.0;
    for (int i = 0; i < n; ++i) {
      if (b->cells[i].revealed || a->cells[i].forced_mine) {
        continue;
      }
      double p = a->cells[i].mine_prob;
      if (p < pmin) {
        pmin = p;
      }
    }
    out->safest_pct = (pmin <= 1.0) ? reasoning_pct(pmin) : 0;
    out->took_riskier = (a->cells[idx].mine_prob > pmin + 1e-9);
  }

  if (hover_x >= 0 && hover_y >= 0 && hover_x < b->width &&
      hover_y < b->height) {
    int idx = game_index(b, hover_x, hover_y);
    if (!b->cells[idx].revealed) {
      out->hover_valid = true;
      out->hover_x = hover_x;
      out->hover_y = hover_y;
      out->hover_pct = reasoning_pct(a->cells[idx].mine_prob);
      out->hover_forced_safe = a->cells[idx].forced_safe;
      out->hover_forced_mine = a->cells[idx].forced_mine;
      out->hover_frontier = a->cells[idx].is_frontier;
      out->hover_gain = a->cells[idx].info_gain;
    }
  }
}
