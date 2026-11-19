/* ui_req.h: the requests game code makes of the UI (Polaris 1.3). Today each forwards at once to the UI function it
 * names, in the same frame with the same result; the call is counted when made from a simulate phase ("frame:" line,
 * src/frame.h). The screen stack (Polaris 7.1) turns these into a queue drained in the emit phase, and the Lab stubs
 * them here. Queries (menu_open(), drb_ready(), hud_boss_bar_entity()) stay direct calls. */
#ifndef UI_REQ_H
#define UI_REQ_H

#include "drb.h"
#include "frame.h"
#include "hud.h"

/* pickup.c's item-get dialog (menu.drb F25) */
static inline void ui_req_dialog_open(const char *screen, int rows) { frame_ui_request(), drb_open(screen, rows); }
static inline int ui_req_dialog_step(void) { return frame_ui_request(), drb_step(); }
static inline void ui_req_dialog_close(void) { frame_ui_request(), drb_close(); }
/* event.c: the boss's health bar (EMEVD) */
static inline void ui_req_boss_bar(int slot, int entity, int name) { frame_ui_request(), hud_boss_bar(slot, entity, name); }

#endif
