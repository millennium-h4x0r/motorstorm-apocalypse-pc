/* MotorStorm Apocalypse port: user/settings.ini -- see settings.h. */
#include "settings.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <dirent.h>
#include <algorithm>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#endif

/* The language pack names, as the runtime's pack redirect
 * (runtime/ppu/ppu_fs.cpp, lang_redirect) and ui.cpp know them. Inherited
 * from inFAMOUS 2; revisit for MotorStorm once its disc's language layout is
 * known. */
static const char* const k_langs[] = {
    "brazilian", "english", "latino", "spanish", "french", "german", "italian",
    "portuguese", "russian", "polish", "dutch", "turkish", "japanese",
    "british", "korean", "chinese",
};

static int lang_known(const char* v)
{
    for (const char* l : k_langs)
        if (!strcmp(v, l)) return 1;
    return 0;
}

/* The last three have text and no voices. */
static int voice_known(const char* v)
{
    return lang_known(v) && strcmp(v, "british") && strcmp(v, "korean") && strcmp(v, "chinese");
}

static port_settings s_file;    /* as read, for the pinned fields when saving */
static unsigned s_pinned;

void port_settings_defaults(port_settings* s)
{
    memset(s, 0, sizeof *s);
    s->scale = 1;
    s->fullscreen = 0;
    s->vsync = 0;
    s->stretch = 0;
    s->display = 0;
    s->window_w = s->window_h = 0;
    s->filter = 0;
    s->sharpen = 0;
    s->brightness = 0;
    s->gamma = 100;
    snprintf(s->voice, sizeof s->voice, "english");
    snprintf(s->text, sizeof s->text, "english");
    s->fps_limit = 0;
    s->fps_counter = 0;
    s->frame_rate = 60;
    s->record_thread = 1;
    s->skip_launcher = 0;
    s->fullres_particles = 1;
    s->aniso = 16;
    s->backup_on_play = 1;
    s->keep_backups = 8;
}

static std::string trim(const std::string& t)
{
    size_t a = t.find_first_not_of(" \t\r\n"), b = t.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
}

/* The window's size as the file and PS3RECOMP_VK_WINDOW_SIZE write it:
 * "auto" (the picture's own size), "fit" (the screen) or <w>x<h> pixels.
 * Anything else leaves the old value. */
static void set_window(int* w, int* h, const char* v)
{
    int a = 0, b = 0;
    char tail = 0;
    if (!strcmp(v, "auto")) *w = *h = 0;
    else if (!strcmp(v, "fit")) *w = *h = -1;
    else if (sscanf(v, "%dx%d%c", &a, &b, &tail) == 2 && a >= 320 && a <= 16384 && b >= 180 && b <= 16384) {
        *w = a; *h = b;
    }
}

static std::string window_text(const port_settings* s)
{
    if (s->window_w < 0) return "fit";
    if (s->window_w == 0 || s->window_h == 0) return "auto";
    return std::to_string(s->window_w) + "x" + std::to_string(s->window_h);
}

/* An integer in [lo, hi], else the old value. */
static void set_int(int* dst, const std::string& v, int lo, int hi)
{
    char* end = nullptr;
    errno = 0;
    const long n = strtol(v.c_str(), &end, 10);
    if (errno || end == v.c_str() || *end || n < lo || n > hi) return;
    *dst = (int)n;
}

int port_settings_load(port_settings* s, const char* path)
{
    port_settings_defaults(s);
    FILE* f = path ? fopen(path, "r") : nullptr;
    if (!f) return 0;
    std::string section;
    char line[2048];
    bool first = true;
    while (fgets(line, sizeof line, f)) {
        std::string l = trim(line);
        /* A byte order mark, from an editor that saves UTF-8 that way. */
        if (first && l.compare(0, 3, "\xEF\xBB\xBF") == 0) l = trim(l.substr(3));
        first = false;
        if (l.empty() || l[0] == ';' || l[0] == '#') continue;
        if (l[0] == '[') {
            const size_t e = l.find(']');
            section = e == std::string::npos ? std::string() : trim(l.substr(1, e - 1));
            continue;
        }
        const size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trim(l.substr(0, eq));
        std::string v = trim(l.substr(eq + 1));
        const std::string whole = v;    /* a path may hold ; or # */
        const size_t c = v.find_first_of(";#");
        if (c != std::string::npos) v = trim(v.substr(0, c));
        if (section == "video") {
            if (k == "scale") set_int(&s->scale, v, 1, 4);
            else if (k == "window") set_window(&s->window_w, &s->window_h, v.c_str());
            else if (k == "filter") { if (v == "nearest") s->filter = 1; else if (v == "linear") s->filter = 0; }
            else if (k == "sharpen") set_int(&s->sharpen, v, 0, 100);
            else if (k == "brightness") set_int(&s->brightness, v, -50, 50);
            else if (k == "gamma") set_int(&s->gamma, v, 50, 200);
            else if (k == "fullscreen") set_int(&s->fullscreen, v, 0, 1);
            else if (k == "vsync") set_int(&s->vsync, v, 0, 1);
            else if (k == "stretch") set_int(&s->stretch, v, 0, 1);
            else if (k == "display") set_int(&s->display, v, 0, 15);
        } else if (section == "language") {
            if (k == "voice" && voice_known(v.c_str())) snprintf(s->voice, sizeof s->voice, "%s", v.c_str());
            else if (k == "text" && lang_known(v.c_str())) snprintf(s->text, sizeof s->text, "%s", v.c_str());
        } else if (section == "performance") {
            if (k == "fps_limit") set_int(&s->fps_limit, v, 0, 1000);
            else if (k == "fps_counter") set_int(&s->fps_counter, v, 0, 2);
            else if (k == "frame_rate") { int n = 60; set_int(&n, v, 30, 60); s->frame_rate = n == 30 ? 30 : 60; }
            else if (k == "record_thread") set_int(&s->record_thread, v, 0, 1);
        } else if (section == "launcher") {
            if (k == "skip") set_int(&s->skip_launcher, v, 0, 1);
        } else if (section == "game") {
            if (k == "path") snprintf(s->game_path, sizeof s->game_path, "%s", whole.c_str());
        } else if (section == "mods") {
            if (k == "fullres_particles") set_int(&s->fullres_particles, v, 0, 1);
            else if (k == "aniso") set_int(&s->aniso, v, 0, 16);
            else if (k == "enabled") snprintf(s->mods_on, sizeof s->mods_on, "%s", v.c_str());
        } else if (section == "saves") {
            if (k == "backup_on_play") set_int(&s->backup_on_play, v, 0, 1);
            else if (k == "keep_backups") set_int(&s->keep_backups, v, 1, 99);
        }
    }
    fclose(f);
    return 1;
}

int port_settings_save(const port_settings* in, const char* path)
{
    if (!path) return -1;
    port_settings out = *in;
    const port_settings* s = &out;
    if (s_pinned & PORT_PIN_SCALE)         out.scale = s_file.scale;
    if (s_pinned & PORT_PIN_FULLSCREEN)    out.fullscreen = s_file.fullscreen;
    if (s_pinned & PORT_PIN_VSYNC)         out.vsync = s_file.vsync;
    if (s_pinned & PORT_PIN_STRETCH)       out.stretch = s_file.stretch;
    if (s_pinned & PORT_PIN_DISPLAY)       out.display = s_file.display;
    if (s_pinned & PORT_PIN_WINDOW_SIZE)   { out.window_w = s_file.window_w; out.window_h = s_file.window_h; }
    if (s_pinned & PORT_PIN_FILTER)        out.filter = s_file.filter;
    if (s_pinned & PORT_PIN_SHARPEN)       out.sharpen = s_file.sharpen;
    if (s_pinned & PORT_PIN_BRIGHTNESS)    out.brightness = s_file.brightness;
    if (s_pinned & PORT_PIN_GAMMA)         out.gamma = s_file.gamma;
    if (s_pinned & PORT_PIN_VOICE)         memcpy(out.voice, s_file.voice, sizeof out.voice);
    if (s_pinned & PORT_PIN_TEXT)          memcpy(out.text, s_file.text, sizeof out.text);
    if (s_pinned & PORT_PIN_FPS_LIMIT)     out.fps_limit = s_file.fps_limit;
    if (s_pinned & PORT_PIN_RECORD_THREAD) out.record_thread = s_file.record_thread;
    if (s_pinned & PORT_PIN_PARTICLES)     out.fullres_particles = s_file.fullres_particles;
    if (s_pinned & PORT_PIN_ANISO)         out.aniso = s_file.aniso;
    if (s_pinned & PORT_PIN_FRAME_RATE)    out.frame_rate = s_file.frame_rate;
    if (s_pinned & PORT_PIN_GAME)          memcpy(out.game_path, s_file.game_path, sizeof out.game_path);
    std::string dir = path;
    const size_t slash = dir.find_last_of("/\\");
    if (slash != std::string::npos) {
        dir.resize(slash);
#ifdef _WIN32
        if (!dir.empty()) _mkdir(dir.c_str());
#else
        if (!dir.empty()) mkdir(dir.c_str(), 0755);
#endif
    }
    /* Written beside and renamed into place: a crash mid-write leaves the
     * old file, not half of a new one. */
    const std::string tmp = std::string(path) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return -1;
    fprintf(f,
            "; MotorStorm Apocalypse port settings. A variable set in the environment wins over\n"
            "; this file. The launcher and the in-game menu (F1) write it.\n"
            "\n[video]\n"
            "scale = %d          ; 1 = 720p, 2 = 1440p, 3 = 4K, 4 = 5K\n"
            "fullscreen = %d\n"
            "vsync = %d\n"
            "stretch = %d        ; 0 = black bars, 1 = stretch\n"
            "display = %d\n"
            "window = %s      ; the window's size: auto (the picture's), fit (the screen) or <w>x<h> pixels\n"
            "filter = %s      ; scaling to the window: linear (smooth) or nearest\n"
            "sharpen = %d        ; 0 = off .. 100\n"
            "brightness = %d     ; -50 .. 50\n"
            "gamma = %d        ; in hundredths, 50 .. 200; 100 = as the game draws\n"
            "\n[language]\n"
            "voice = %s\n"
            "text = %s\n"
            "\n[performance]\n"
            "fps_limit = %d      ; 0 = none\n"
            "fps_counter = %d    ; performance HUD: 0 off, 1 detailed, 2 compact (F10)\n"
            "frame_rate = %d     ; 60, or 30 (every second refresh, as on the PS3)\n"
            "record_thread = %d\n"
            "\n[launcher]\n"
            "skip = %d\n"
            "\n[mods]\n"
            "fullres_particles = %d ; particle effects at the picture's resolution\n"
            "aniso = %d             ; 0 = the game's own, 2..16\n"
            "enabled = %s ; mod folders under mods/, in load order (later wins)\n",
            s->scale, s->fullscreen, s->vsync, s->stretch, s->display,
            window_text(s).c_str(), s->filter ? "nearest" : "linear", s->sharpen, s->brightness, s->gamma,
            s->voice, s->text, s->fps_limit, s->fps_counter, s->frame_rate, s->record_thread,
            s->skip_launcher, s->fullres_particles, s->aniso, s->mods_on);
    /* The whole line after "path =" is the path: no comment on it. */
    fprintf(f, "\n[game]\n"
               "; where the game is: the folder that holds PS3_GAME. Empty: disc/\n"
               "path = %s\n", s->game_path);
    fprintf(f,
            "\n[saves]\n"
            "backup_on_play = %d ; back up the saves each time Play is pressed\n"
            "keep_backups = %d   ; how many automatic backups to keep (1..99)\n",
            s->backup_on_play, s->keep_backups);
    const int ok = fclose(f) == 0;
#ifdef _WIN32
    /* The CRT's rename() refuses an existing target. */
    if (!ok || !MoveFileExA(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING)) { remove(tmp.c_str()); return -1; }
#else
    if (!ok || rename(tmp.c_str(), path)) { remove(tmp.c_str()); return -1; }
#endif
    return 0;
}

static std::string s_exported[64];
static int s_nexported;

static void put(const char* name, const char* value)
{
    if (getenv(name)) return;
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
    if (s_nexported < 64) s_exported[s_nexported++] = name;
}

/* The backend's reading of a switch: set, not empty, not "0". */
static int env_on(const char* v) { return v[0] && v[0] != '0'; }

/* The backend's reading of a number: clamped to [lo, hi], `def` when it is
 * not one. */
static int env_range(const char* v, int def, int lo, int hi)
{
    char* end = nullptr;
    const long n = strtol(v, &end, 10);
    if (end == v) return def;
    return n < lo ? lo : n > hi ? hi : (int)n;
}

unsigned port_settings_from_env(port_settings* s)
{
    s_file = *s;
    unsigned pin = 0;
    const char* v;
    if ((v = getenv("PS3RECOMP_RES_SCALE"))) {
        const int n = atoi(v);
        if (n >= 1 && n <= 4) { s->scale = n; pin |= PORT_PIN_SCALE; }
    }
    if ((v = getenv("PS3RECOMP_VK_FULLSCREEN"))) { s->fullscreen = env_on(v); pin |= PORT_PIN_FULLSCREEN; }
    if ((v = getenv("PS3RECOMP_VK_PRESENT")))    { s->vsync = !strcmp(v, "fifo"); pin |= PORT_PIN_VSYNC; }
    if ((v = getenv("PS3RECOMP_VK_STRETCH")))    { s->stretch = env_on(v); pin |= PORT_PIN_STRETCH; }
    if ((v = getenv("PS3RECOMP_VK_DISPLAY")))    { s->display = atoi(v) < 0 ? 0 : atoi(v); pin |= PORT_PIN_DISPLAY; }
    if ((v = getenv("PS3RECOMP_VK_WINDOW_SIZE"))) {
        s->window_w = s->window_h = 0;              /* as the backend: anything unknown is the picture's size */
        set_window(&s->window_w, &s->window_h, v);
        pin |= PORT_PIN_WINDOW_SIZE;
    }
    if ((v = getenv("PS3RECOMP_VK_FILTER")))     { s->filter = !strcmp(v, "nearest"); pin |= PORT_PIN_FILTER; }
    if ((v = getenv("PS3RECOMP_VK_SHARPEN")))    { s->sharpen = env_range(v, 0, 0, 100); pin |= PORT_PIN_SHARPEN; }
    if ((v = getenv("PS3RECOMP_VK_BRIGHTNESS"))) { s->brightness = env_range(v, 0, -50, 50); pin |= PORT_PIN_BRIGHTNESS; }
    if ((v = getenv("PS3RECOMP_VK_GAMMA")))      { s->gamma = env_range(v, 100, 50, 200); pin |= PORT_PIN_GAMMA; }
    if ((v = getenv("PS3_VOICE_LANG")) && voice_known(v)) {
        snprintf(s->voice, sizeof s->voice, "%s", v); pin |= PORT_PIN_VOICE;
    }
    if ((v = getenv("PS3_TEXT_LANG")) && lang_known(v)) {
        snprintf(s->text, sizeof s->text, "%s", v); pin |= PORT_PIN_TEXT;
    }
    if ((v = getenv("PS3RECOMP_FPS_LIMIT")))     { s->fps_limit = atoi(v) < 0 ? 0 : atoi(v); pin |= PORT_PIN_FPS_LIMIT; }
    if ((v = getenv("PS3_SIM_FPS")))             { s->frame_rate = atoi(v) == 30 ? 30 : 60; pin |= PORT_PIN_FRAME_RATE; }
    if ((v = getenv("PS3RECOMP_BE_THREAD")))     { s->record_thread = env_on(v); pin |= PORT_PIN_RECORD_THREAD; }
    if ((v = getenv("PS3RECOMP_FULLRES_PARTICLES"))) { s->fullres_particles = env_on(v); pin |= PORT_PIN_PARTICLES; }
    if ((v = getenv("PS3_GAME_DIR")) && *v)      { snprintf(s->game_path, sizeof s->game_path, "%s", v); pin |= PORT_PIN_GAME; }
    if ((v = getenv("PS3RECOMP_ANISO")))         { s->aniso = atoi(v) < 0 ? 0 : atoi(v) > 16 ? 16 : atoi(v); pin |= PORT_PIN_ANISO; }
    s_pinned = pin;
    return pin;
}

unsigned port_settings_pinned(void) { return s_pinned; }

extern char** environ;

char** port_settings_restart_env(const char* extra)
{
    size_t n = 0;
    while (environ[n]) n++;
    char** env = (char**)calloc(n + 3, sizeof *env);
    if (!env) return nullptr;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        const char* e = environ[i];
        const char* eq = strchr(e, '=');
        const size_t len = eq ? (size_t)(eq - e) : strlen(e);
        bool drop = len == 17 && !strncmp(e, "PS3_SKIP_LAUNCHER", 17);
        for (int j = 0; j < s_nexported && !drop; j++)
            drop = s_exported[j].size() == len && !strncmp(e, s_exported[j].c_str(), len);
        if (!drop) env[k++] = (char*)e;
    }
    env[k++] = (char*)"PS3_SKIP_LAUNCHER=1";
    if (extra) env[k++] = (char*)extra;
    env[k] = nullptr;
    return env;
}

void port_settings_export(const port_settings* s)
{
    char b[32];
    snprintf(b, sizeof b, "%d", s->scale);         put("PS3RECOMP_RES_SCALE", b);
    put("PS3RECOMP_VK_FULLSCREEN", s->fullscreen ? "1" : "0");
    put("PS3RECOMP_VK_PRESENT", s->vsync ? "fifo" : "mailbox");
    put("PS3RECOMP_VK_STRETCH", s->stretch ? "1" : "0");
    snprintf(b, sizeof b, "%d", s->display);       put("PS3RECOMP_VK_DISPLAY", b);
    put("PS3RECOMP_VK_WINDOW_SIZE", window_text(s).c_str());
    put("PS3RECOMP_VK_FILTER", s->filter ? "nearest" : "linear");
    snprintf(b, sizeof b, "%d", s->sharpen);       put("PS3RECOMP_VK_SHARPEN", b);
    snprintf(b, sizeof b, "%d", s->brightness);    put("PS3RECOMP_VK_BRIGHTNESS", b);
    snprintf(b, sizeof b, "%d", s->gamma);         put("PS3RECOMP_VK_GAMMA", b);
    put("PS3_VOICE_LANG", s->voice);
    put("PS3_TEXT_LANG", s->text);
    /* The console language (cellSysutil, PS3_SYS_LANG) follows the text
     * language, so the game's own language logic matches it. A pack with no
     * console language of its own maps to the nearest one. Unknown: English.
     * (inFAMOUS 2 needed brazilian -> pt, as it treated pt-br (17) as unknown;
     * check how MotorStorm treats the late codes pt-br, en-gb and tr.) */
    static const struct { const char* pack; const char* sys; } k_sys[] = {
        { "english", "en" }, { "british", "en-gb" }, { "french", "fr" }, { "german", "de" },
        { "italian", "it" }, { "spanish", "es" }, { "latino", "es" }, { "portuguese", "pt" },
        { "brazilian", "pt-br" }, { "dutch", "nl" }, { "russian", "ru" }, { "polish", "pl" },
        { "turkish", "tr" }, { "japanese", "ja" }, { "korean", "ko" }, { "chinese", "zh-t" },
    };
    const char* sys = "en";
    for (const auto& m : k_sys)
        if (!strcmp(s->text, m.pack)) { sys = m.sys; break; }
    put("PS3_SYS_LANG", sys);
    snprintf(b, sizeof b, "%d", s->fps_limit);     put("PS3RECOMP_FPS_LIMIT", b);
    put("PS3RECOMP_BE_THREAD", s->record_thread ? "1" : "0");
    put("PS3_SIM_FPS", s->frame_rate == 30 ? "30" : "60");
    /* The GPU reads the guest's vertex bytes itself: in inFAMOUS 2 the CPU
     * fetch it replaces made turning the camera drop to ~40 fps in the city,
     * where most geometry was rebuilt by the SPUs every frame. */
    /* Vertex pulling (the GPU decodes the guest's vertex bytes) came from
     * inFAMOUS 2. In MotorStorm it costs: 1P race at 1440p 46.7 fps with a
     * 54 ms p90 (stutter) against 60.0 / 17.7 ms without (2026-11-11); the
     * CPU decode it replaces gives the same vertices. */
    put("PS3RECOMP_ENG_VPULL", "0");
    snprintf(b, sizeof b, "%d", s->aniso);         put("PS3RECOMP_ANISO", b);
    put("PS3RECOMP_FULLRES_PARTICLES", s->fullres_particles ? "1" : "0");
    if (s->skip_launcher) put("PS3_SKIP_LAUNCHER", "1");
    /* The enabled mods that are there, as the paths the filesystem overlays
     * (runtime/ppu/ppu_fs.cpp, mod_overlay). */
    std::string mods;
    const std::string list = s->mods_on;
    for (size_t p = 0; p < list.size();) {
        size_t q = list.find(',', p);
        if (q == std::string::npos) q = list.size();
        const std::string d = trim(list.substr(p, q - p));
        p = q + 1;
        if (d.empty() || d.find('/') != std::string::npos || d.find('\\') != std::string::npos || d[0] == '.') continue;
        const std::string full = std::string(port_mods_dir()) + "/" + d;
        struct stat st;
        if (stat((full + "/files").c_str(), &st) != 0) continue;
        if (!mods.empty()) mods += "|";
        mods += full;
    }
    if (!mods.empty()) {
        static std::string keep;
        keep = mods;
        put("PS3_MODS", keep.c_str());
    }
}

const char* port_mods_dir(void)
{
    static std::string p;
    if (p.empty()) {
        const char* d = getenv("PS3_MODS_DIR");
        p = d && *d ? d : "mods";
    }
    return p.c_str();
}

int port_mods_scan(port_mod* out, int cap)
{
    int n = 0;
    DIR* d = opendir(port_mods_dir());
    if (!d) return 0;
    while (struct dirent* e = readdir(d)) {
        if (e->d_name[0] == '.' || n >= cap) continue;
        const std::string base = std::string(port_mods_dir()) + "/" + e->d_name;
        struct stat st;
        if (stat(base.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        port_mod* m = &out[n++];
        memset(m, 0, sizeof *m);
        snprintf(m->dir, sizeof m->dir, "%s", e->d_name);
        snprintf(m->name, sizeof m->name, "%s", e->d_name);
        if (FILE* f = fopen((base + "/mod.ini").c_str(), "r")) {
            char line[512];
            while (fgets(line, sizeof line, f)) {
                std::string l = line;
                const size_t eq = l.find('=');
                if (eq == std::string::npos) continue;
                const std::string k = trim(l.substr(0, eq)), v = trim(l.substr(eq + 1));
                if (k == "name" && !v.empty()) snprintf(m->name, sizeof m->name, "%s", v.c_str());
                else if (k == "author") snprintf(m->author, sizeof m->author, "%s", v.c_str());
                else if (k == "version") snprintf(m->version, sizeof m->version, "%s", v.c_str());
                else if (k == "description") snprintf(m->description, sizeof m->description, "%s", v.c_str());
            }
            fclose(f);
        }
    }
    closedir(d);
    std::sort(out, out + n, [](const port_mod& a, const port_mod& b) { return strcmp(a.dir, b.dir) < 0; });
    return n;
}

int port_mod_enabled(const port_settings* s, const char* dir)
{
    const std::string list = std::string(",") + s->mods_on + ",";
    std::string needle = std::string(",") + dir + ",";
    std::string norm;
    for (char c : list) if (c != ' ') norm += c;
    return norm.find(needle) != std::string::npos;
}

void port_mod_set(port_settings* s, const char* dir, int on)
{
    std::string out;
    const std::string list = s->mods_on;
    for (size_t p = 0; p < list.size();) {
        size_t q = list.find(',', p);
        if (q == std::string::npos) q = list.size();
        const std::string d = trim(list.substr(p, q - p));
        p = q + 1;
        if (d.empty() || d == dir) continue;
        if (!out.empty()) out += ",";
        out += d;
    }
    if (on) { if (!out.empty()) out += ","; out += dir; }
    snprintf(s->mods_on, sizeof s->mods_on, "%s", out.c_str());
}

const char* port_settings_path(void)
{
    static std::string p;
    if (p.empty()) {
        const char* d = getenv("PS3_USER_DIR");
        p = std::string(d && *d ? d : "user") + "/settings.ini";
    }
    return p.c_str();
}

port_settings* port_settings_get(void)
{
    static port_settings s;
    static int loaded;
    if (!loaded) { loaded = 1; port_settings_defaults(&s); }
    return &s;
}
