/* MotorStorm Apocalypse port: where the player's game is, and whether what the
 * port needs at run time is there. No UI and no runtime in here: main.cpp
 * asks before it loads anything, ui_game.cpp shows the answer, and
 * tests/gamefiles_test.cpp runs it on made-up trees.
 *
 * The game's code is built into the binary, lifted from one executable
 * (BCES-00484 version 01.00, scripts/lift.sh): the location only says where
 * the game's data and that same EBOOT.elf are. */
#ifndef PORT_GAMEFILES_H
#define PORT_GAMEFILES_H

#include <string>
#include <vector>

namespace gamefiles {

/* The executable the port is lifted from (scripts/lift.sh, docs/REQUIRED_FILES.md). */
extern const char* const k_eboot_sha256;
extern const unsigned long long k_eboot_size;
extern const char* const k_title_id;        /* "BCES00484" */

enum State {
    FOUND,      /* there, and the right one */
    WARNING,    /* the game starts without it, with something missing */
    MISSING,    /* not there: the game cannot start */
    WRONG,      /* there, but not the one the port needs: the game cannot start */
};

struct Item {
    std::string id;         /* "folder", "sfo", "eboot", "data", "media", "firmware", "space" */
    std::string name;       /* the line's title */
    State state;
    std::string detail;     /* one line: what was found, or what is wrong */
    std::string help;       /* what to do about it; empty when nothing is to be done */
};

/* The places outside the game's folder a check looks at. */
struct Where {
    std::string dev_flash;  /* the PS3 system software's folder; empty: none found */
    std::string saves;      /* where the saves go */
    std::string cache;      /* /dev_hdd1: the cache the game builds on its first start */
};
/* From the environment, as main.cpp and the runtime decide them:
 * PS3_DEV_FLASH or RPCS3's own folder, PS3_SAVEDATA_ROOT, PS3_HDD1_ROOT. */
Where where_from_env();

struct Report {
    std::string given;      /* the path as asked */
    std::string root;       /* the folder holding PS3_GAME */
    std::string usrdir;     /* <root>/PS3_GAME/USRDIR */
    std::string eboot;      /* <usrdir>/EBOOT.elf */
    std::vector<Item> items;
    bool ready = false;     /* nothing MISSING or WRONG */
    std::string problem;    /* the first thing in the way, one line; empty when ready */
    const Item* find(const char* id) const;
};

/* A path as a person types or pastes it: quotes, file://, a leading ~, spaces
 * around it and slashes at its end taken off. */
std::string clean_path(const std::string& typed);

/* The folder holding PS3_GAME, from any of: that folder, PS3_GAME, USRDIR, or
 * a file in USRDIR (EBOOT.elf). Lexical: links are not followed. A path that
 * is none of them comes back as it is. */
std::string root_of(const std::string& path);

/* Everything the port needs, for the game at `path` (any form root_of takes). */
Report check(const std::string& path, const Where& where);

/* SHA-256 of a file as 64 hex digits; empty when it cannot be read. */
std::string sha256_file(const std::string& path);

/* Which rule chose the location, in the order they are tried. */
enum Source {
    FROM_ARGUMENT,      /* the first argument that is not an option, when it exists */
    FROM_ENVIRONMENT,   /* PS3_GAME_DIR */
    FROM_SETTINGS,      /* [game] path in settings.ini */
    FROM_DEFAULT,       /* disc/ */
};
struct Boot {
    Source source = FROM_DEFAULT;
    std::string given;
    Report report;
};
/* main(), once, before anything reads the game: choose the location and check
 * it. `settings_path` is settings.ini's [game] path, NULL or empty for none. */
const Boot& resolve(int argc, char** argv, const char* settings_path);
/* What resolve() found (a default-made Boot before it ran). */
const Boot& boot();

} /* namespace gamefiles */

#endif
