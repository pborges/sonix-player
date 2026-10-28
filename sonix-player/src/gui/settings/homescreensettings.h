#ifndef HOMESCREENSETTINGS_H
#define HOMESCREENSETTINGS_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Settings > More > Homescreen: the main menu's tiles, the More page's and the
// hidden ones, in three lists, moved between them and reordered by dragging;
// under them, a switch for each tile inside Streaming and Wireless. Hiding a
// tile only leaves it out of its grid; see homescreen.h.
//
// The screen is created with the others in gui_init(), so the row on the More
// page can point at it; its rows are made here, after every grid has
// registered its tiles.

extern lv_obj_t *homescreensettings_screen;

void homescreensettings_init(gui_config_t *cfg);

#endif /* HOMESCREENSETTINGS_H */
