#ifndef ABSPAGE_H
#define ABSPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// What the Sync tile on the Audiobooks page opens: the Audiobookshelf server's
// books, series and authors, a search, and a page for each book. The account
// itself is in the Audiobooks options (abssettings.h), never here.
void abspage_init(gui_config_t *cfg);

// Opens the home page.
void abspage_open(void);

#endif /* ABSPAGE_H */
