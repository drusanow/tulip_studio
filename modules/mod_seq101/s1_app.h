// s1_app.h - SEQ101's state, shared by the module glue (s1_mod.c) and its
// UI (ui/s1_ui.c).
//
// The UI (core 0) owns g_s1 and edits it a field at a time; the rt task
// reads it on every 1/16. A step is a few bytes, so a read that lands
// between two writes plays at worst one odd note - never a crash, never a
// wait.
#pragma once

#include "core/s1_model.h"
#include "studio/studio.h"

extern s1_state_t g_s1;

// the step each lane is playing (-1 stopped), for the playhead
int s1_play_step(int lane);
// queue a pattern for the lane's next loop crossover (-1: cancel); waiting?
void s1_queue(int lane, int slot);
int s1_queued(int lane);
// play a pattern from now (its step carries on)
void s1_activate(int lane, int slot);

// something changed: kept in /files/seq101/state.json a moment later
void s1_changed(void);

// projects: the whole state under a name, in /files/seq101/projects (UI task)
#define S1_PROJ_NAME 24
#define S1_MAX_PROJ 24
int s1_proj_list(char (*names)[S1_PROJ_NAME + 1], int max);   // sorted; returns how many
bool s1_proj_save(const char *name);     // new, or over the one of that name
bool s1_proj_load(const char *name);     // replaces everything; false: left as it was
bool s1_proj_delete(const char *name);

// the CV: who has the DACs, as a line for the UI
void s1_cv_status(char *out, int n);
