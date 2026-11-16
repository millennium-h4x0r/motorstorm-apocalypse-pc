/* MotorStorm Apocalypse port. The player's saves and their backups: list,
 * back up, prune, restore, delete, export to a .zip, import one. Plain C++
 * (std::filesystem), no UI: ui_saves.cpp draws the page, tests/saves_test.cpp
 * runs every path on scratch folders.
 *
 * Folders
 *   saves    $PS3_SAVEDATA_ROOT, or gamedata/dev_hdd0/home/00000001/savedata:
 *            one folder a save (BCES00484/PARAM.SFO, ICON0.PNG, the data).
 *   backups  <$PS3_USER_DIR or user>/backups/<YYYYmmdd-HHMMSS>-<reason>/, each
 *            a copy of the saves folder's contents. The reason is "play"
 *            (made when Play was pressed), "restore" (the saves as they were
 *            before a restore), "manual" or "import". A folder named with the
 *            time alone (what the port wrote before) is a manual one.
 *   exports  <$PS3_USER_DIR or user>/exports/, the .zip files
 *
 * Rules
 *   - A backup is copied beside its place under a dotted name and renamed, so
 *     a crash leaves no half backup in the list.
 *   - An automatic backup ("play") is skipped when the saves are the same,
 *     file for file, as the newest backup. A manual one is always made.
 *   - Pruning removes automatic backups ("play", "restore") beyond the newest
 *     `keep`. Manual and imported ones go only when the player deletes them.
 *   - A restore first backs up the saves as they are ("restore"), then swaps
 *     the whole saves folder for a copy of the backup (two renames). It is
 *     refused while the game runs: the game holds its save in memory and
 *     would write it back over the restored one.
 *   - An archive is untrusted: see import_archive.
 *
 * Nothing here follows or copies a symbolic link, and only regular files and
 * folders are read. Every call is synchronous; MotorStorm's saves are about
 * 100 KB (a backup takes a few milliseconds) and an import is capped at
 * k_max_total bytes. */
#ifndef PORT_SAVES_H
#define PORT_SAVES_H

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace saves {

struct Config {
    std::string save_root;      /* the saves folder */
    std::string backup_root;    /* .../backups */
    std::string export_root;    /* .../exports */
};
/* From the environment, as described above. */
Config default_config();

struct SaveInfo {
    std::string dir;            /* folder name, e.g. BCES00484 */
    std::string title, subtitle, detail;    /* PARAM.SFO; the title is the folder name without one */
    std::string icon;           /* path of ICON0.PNG, empty without one */
    uint64_t size = 0;          /* bytes, all files */
    int files = 0;
    std::time_t mtime = 0;      /* the newest file */
};
std::vector<SaveInfo> list_saves(const std::string& root);

struct BackupInfo {
    std::string name;           /* folder name under the backups folder */
    std::string reason;         /* "play", "restore", "manual", "import" */
    std::time_t time = 0;       /* from the name; the folder's time when it has none */
    uint64_t size = 0;
    int saves = 0;              /* save folders inside */
    bool automatic = false;     /* pruned by `keep` */
};
/* Newest first. */
std::vector<BackupInfo> list_backups(const Config& c);

struct Result {
    bool ok = false;
    bool skipped = false;       /* ok, and nothing was made: no saves, or nothing changed */
    std::string message;        /* one line for the player */
    std::string path;           /* what was made: the backup's folder name, or the archive's path */
};

/* reason: "manual", "play", "restore" or "import" (anything else: "manual").
 * No saves: ok and skipped. reason "play" and the saves unchanged since the
 * newest backup: ok and skipped. */
Result backup(const Config& c, const char* reason);
/* Remove automatic backups beyond the newest `keep` (at least 1). Returns how
 * many went. */
int prune(const Config& c, int keep);
/* What Play does: backup(c, "play"), then prune(c, keep). */
Result backup_for_play(const Config& c, int keep);

/* Replace the saves with backup `name`. Refused when game_running. */
Result restore(const Config& c, const std::string& name, bool game_running);
Result remove_backup(const Config& c, const std::string& name);

/* A .zip of backup `name`, or of the saves as they are when `name` is empty,
 * into the exports folder: MotorStormApocalypse-saves-<time>[-<reason>].zip.
 * Stored, not compressed: any unzip reads it. */
Result export_archive(const Config& c, const std::string& name);

/* The limits an archive must keep. */
constexpr uint64_t k_max_archive = 64ull << 20;    /* the .zip itself */
constexpr uint64_t k_max_total = 128ull << 20;     /* what it unpacks to */
constexpr unsigned k_max_entries = 2048;
/* A .zip as a new backup ("import"); the saves themselves are not touched,
 * the player restores it like any other. The archive is checked before
 * anything is written and refused as a whole on the first fault:
 *   - a plain .zip only: no zip64, no encryption, no multi-part; stored or
 *     deflated entries; sizes within the limits above; CRCs must match;
 *   - every name is <save folder>/<file> (one wrapping folder is accepted
 *     and dropped): relative, no "..", no drive or absolute path, letters,
 *     digits and " ._-" only, no Windows device names, no duplicates;
 *   - no symbolic links or other special entries;
 *   - at least one <save folder>/PARAM.SFO.
 * It unpacks into a dotted folder inside the backups folder, renamed when
 * complete. */
Result import_archive(const Config& c, const std::string& zip_path);
/* The .zip files in the exports folder, newest first (what Import offers). */
std::vector<std::string> list_archives(const Config& c);

/* Make a folder (and those above it) so that a file manager can open it. */
void ensure_folder(const std::string& path);

/* "15 Nov 2026 21:59" */
std::string format_time(std::time_t t);
/* "812 bytes", "104 KB", "1.2 MB" */
std::string format_size(uint64_t bytes);
/* "before Play", "before restore", "manual", "imported" */
const char* reason_label(const std::string& reason);

} /* namespace saves */

#endif
