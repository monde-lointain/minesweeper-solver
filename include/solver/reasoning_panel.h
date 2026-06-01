/* reasoning_panel.h — ImGui renderer for the companion window's reasoning view.
 * The ImGui boundary lives in the .cc (system include). */
#ifndef SOLVER_REASONING_PANEL_H
#define SOLVER_REASONING_PANEL_H

#include "solver/reasoning.h"

/* Render the reasoning view as ImGui widgets into the current window. Call
 * between ImGui::Begin/End for the companion window. */
void reasoning_panel_draw(const struct ReasoningView* v);

#endif /* SOLVER_REASONING_PANEL_H */
