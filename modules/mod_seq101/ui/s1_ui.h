// s1_ui.h - SEQ101's screen (UI task).
#pragma once

#include <stdbool.h>

#include "lvgl.h"

void s1_ui_create(lv_obj_t *scr);
void s1_ui_poll(bool visible);
void s1_ui_show(bool shown);
void s1_ui_reload(void);   // the state was replaced (a Studio patch): redraw
