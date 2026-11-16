/* The port's screens over the game window (ui.cpp). */
#ifndef PORT_UI_H
#define PORT_UI_H

/* Hook the in-game menu, the FPS counter and the start screen into rsx_ui.
 * After the settings are loaded, before the rsx thread starts. argv is kept
 * for starting the process again with new settings. */
void port_ui_init(int argc, char** argv);

/* Whether the game is not ready (gamefiles.h) and the UI will show its set-up
 * screen instead of booting: 0 when it is ready, and when there is no UI to
 * show (no window, or a build without it). After port_ui_init. */
int port_ui_setup(void);

/* The rsx thread, once its backend is up: show the start screen if it is
 * wanted and the UI runs, else let the boot go on. */
void port_ui_backend_ready(void);

/* The main thread, before the guest boots: wait until the start screen's
 * "Play" (returns at once when there is none). */
void port_ui_wait_start(void);

/* Whether the start screen is showing (the boot keeps the window fresh at a
 * calmer pace then). */
int port_ui_start_screen(void);

#endif
