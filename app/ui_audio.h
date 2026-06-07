#pragma once
#include "lvgl/lvgl.h"

/* Modal AUDIO (piano 8 notes + Morse + SOS). Implemente dans ui_audio.c.
 * Callback de bouton : a brancher sur lv_obj_add_event_cb(...) */
void ui_audio_open_e(lv_event_t *e);
