/* The port's UI, shared between its files (ui.cpp and the ui_*.cpp pages).
 * Everything here runs on the window's thread, inside an ImGui frame. */
#ifndef PORT_UI_INTERNAL_H
#define PORT_UI_INTERNAL_H

#include "settings.h"

#include <string>

namespace pui {

/* Write user/settings.ini now (every change is saved at once). */
void save();
/* A wrapped line of help in the dimmed colour, under a widget. */
void hint(const char* text);
/* A row an environment variable decides (PORT_PIN_*): begin/end around the
 * widget grey it out and explain why on hover. */
bool pinned(unsigned bit);
void pin_begin(unsigned bit);
void pin_end(unsigned bit);
/* `t` inside single quotes for the shell. */
std::string sh_quote(const std::string& t);
/* The file manager on a folder. */
void open_folder(const char* path);
/* One line of the start screen's menus: big text, an optional line under it.
 * True when chosen. */
bool menu_item(const char* id, const char* label, const char* sub, float scale = 1.0f);
/* The saves folder, and a copy of it into <user>/backups/<time>; the result
 * line for the UI is last_backup_note(). */
const char* saves_dir();
bool backup_saves();
const char* last_backup_note();
/* The settings as the game started (what a restart would change). */
const port_settings& boot_settings();

/* Pages that live in their own files. Each draws its widgets into the current
 * ImGui window (a tab or a start-screen page); no Begin/End of its own. */
void page_image(port_settings& s);      /* ui_image.cpp: display and picture options */
void page_game(port_settings& s);       /* ui_game.cpp: where the game is, set up, checks */
void page_saves(port_settings& s);      /* ui_saves.cpp: backups, restore, export */

/* ui_saves.cpp: what Play does first when the settings ask for it: back up
 * the saves if they changed, drop the oldest automatic backups (saves.h).
 * Quick, and a failure is logged and shown on the Saves page, never in
 * Play's way. */
void backup_before_play();
/* ui_saves.cpp: the Saves page has a question open (restore, delete): Circle
 * and Escape answer it, and must not also leave the page. */
bool saves_popup_open();

/* ui_game.cpp: whether the game this run started with has everything the
 * port needs (gamefiles.h), and when not, the first thing in the way as one
 * line for the start screen, e.g. "Set up: EBOOT.elf is missing" ("" when
 * ready). Fixed for the run: a new folder takes a restart. */
bool game_ready();
const char* game_problem();
/* ui.cpp: start the process again so that the settings' game folder is used
 * (the argument that named the game is dropped); on its start screen, or
 * straight into the game. Returns only when that failed. */
void restart_for_game(bool to_start_screen);

} /* namespace pui */

#endif
