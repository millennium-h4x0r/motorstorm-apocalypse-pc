# Roadmap

Goal: the whole game playable natively, split-screen included, with the
finish of the inFAMOUS 2 port (port menu, options, up to 4K, a real 60+ fps
simulation).

| # | Milestone | Done when | State |
|---|---|---|---|
| 1 | Lift and link | a binary that starts and reaches the game's entry | ✅ 2026-11-03 |
| 2 | First pixels | boot logos / first intro video on screen | ✅ 2026-11-04 |
| 3 | Menus | the game's main menu navigated with a (virtual) pad | ✅ 2026-11-04 |
| 4 | First race | one race finished start to end | ✅ 2026-11-04 (lighting and anti-aliasing right since) |
| 5 | Full feature set | a Festival event and a 4-player split-screen race complete; every track loads | 🔧 Festival event ✅ (tests/festival.sh); 2- and 4-player races start and render (tests/splitscreen.sh); every track: tests/all_tracks.sh |
| 6 | Polish | port menu, F1 options, resolution scale, fullscreen/letterbox, hang dumps, README screenshots | 🔧 start screen, F1 menu, scale 1-3, full screen (Wayland HiDPI), letterbox, performance HUD, watchdog dumps ✅; screenshots |
| 7 | Performance | steady 60+ fps simulation at 1440p in single-player races | 🔧 1P races 59.5-59.8 fps at 1440p, median frame 16.7 ms (2026-11-11); 4-player split screen ~35 fps |

Testing is automated (virtual pads, recorded input, frame checks): see the
design's "Working mode".
