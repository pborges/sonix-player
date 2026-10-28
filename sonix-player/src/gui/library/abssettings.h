#ifndef ABSSETTINGS_H
#define ABSSETTINGS_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Audiobookshelf page, reached from the Audiobooks options, and its
// sign-in screen.
void abssettings_init(gui_config_t *cfg);

lv_obj_t *abssettings_screen(void);

#endif /* ABSSETTINGS_H */
