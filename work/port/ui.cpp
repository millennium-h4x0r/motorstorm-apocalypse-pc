/* MotorStorm Apocalypse port: the port's screens over the game's window, drawn
 * with Dear ImGui through the toolkit's rsx_ui (libs/video/rsx_ui.h): the start
 * screen (Play / Options / Quit), the in-game menu (F1, or Select+Start held
 * half a second) and the FPS counter. Every change is written to
 * user/settings.ini at once (settings.h); the options marked live apply now,
 * the others at the next start. */
#include "settings.h"
#include "ui.h"

#ifdef PS3RECOMP_UI

#include "imgui.h"
#include "rsx_ui.h"

#include <SDL2/SDL.h>
#ifdef PORT_HAVE_PNG
#include <png.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <filesystem>
#endif

extern "C" void rsx_vulkan_backend_set_fullscreen(int on);
extern "C" void rsx_vulkan_backend_set_vsync(int on);
extern "C" void rsx_vulkan_backend_set_stretch(int on);
extern "C" void rsx_vulkan_backend_set_fps_limit(int fps);
extern "C" int rsx_vulkan_backend_fullscreen(void);
extern "C" unsigned ppu_boot_frames_presented(void);
extern "C" void port_log_drain(void);       /* main.cpp: the log reaches its file */
extern "C" unsigned rsx_vulkan_backend_guest_draws(void);  /* draws in the last frame */
extern "C" void port_sim_set_fps(int fps);  /* ppu_native.cpp: 60 or 30, live */
extern "C" int port_sim_fps(void);

namespace {

/* The language packs, by the names settings.cpp and the runtime's pack
 * redirect (runtime/ppu/ppu_fs.cpp, lang_redirect) use. The list and which
 * have voices are inherited from inFAMOUS 2; revisit for MotorStorm once its
 * disc's language layout is known. */
struct Lang { const char* pack; const char* name; bool voice; };
const Lang k_langs[] = {
    { "english",    "English",                      true },
    { "british",    "English (UK)",                 false },
    { "french",     "French",                       true },
    { "german",     "German",                       true },
    { "italian",    "Italian",                      true },
    { "spanish",    "Spanish (Spain)",              true },
    { "latino",     "Spanish (Latin America)",      true },
    { "portuguese", "Portuguese (Portugal)",        true },
    { "brazilian",  "Portuguese (Brazil)",          true },
    { "dutch",      "Dutch",                        true },
    { "russian",    "Russian",                      true },
    { "polish",     "Polish",                       true },
    { "turkish",    "Turkish",                      true },
    { "japanese",   "Japanese",                     true },
    { "korean",     "Korean (experimental)",        false },
    { "chinese",    "Chinese (experimental)",       false },
};
const char* const k_scales[] = { "720p (1x)", "1440p (2x)", "4K (3x)" };
const int k_fps_limits[] = { 0, 30, 60, 120, -1 };
const char* const k_fps_names[] = { "Unlimited", "30", "60", "120", "Monitor refresh rate" };

port_settings g_boot;          /* as the game started: what a restart would change */
int g_argc; char** g_argv;
std::atomic<int> g_go{1};      /* the main thread may boot the guest */
bool g_want_start = false;     /* the start screen was asked for */
std::atomic<bool> g_start{false};  /* ...and is showing */
double g_fps = 0.0, g_fps_t = 0.0;
unsigned g_fps_frames = 0;
bool g_styled = false;

void style()
{
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 10.0f; s.FrameRounding = 6.0f; s.TabRounding = 6.0f;
    s.GrabRounding = 6.0f; s.PopupRounding = 6.0f;
    s.WindowPadding = ImVec2(22, 18); s.FramePadding = ImVec2(12, 7);
    s.ItemSpacing = ImVec2(12, 12); s.WindowBorderSize = 0.0f;
    ImVec4* c = s.Colors;
    const ImVec4 accent(0.27f, 0.78f, 1.00f, 1.00f);                 /* #46c8ff */
    c[ImGuiCol_WindowBg]        = ImVec4(0.04f, 0.06f, 0.10f, 0.94f);
    c[ImGuiCol_PopupBg]         = ImVec4(0.06f, 0.08f, 0.13f, 0.98f);
    c[ImGuiCol_FrameBg]         = ImVec4(0.11f, 0.14f, 0.20f, 1.00f);
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0.15f, 0.20f, 0.29f, 1.00f);
    c[ImGuiCol_FrameBgActive]   = ImVec4(0.17f, 0.24f, 0.35f, 1.00f);
    c[ImGuiCol_Tab]             = ImVec4(0.10f, 0.13f, 0.19f, 1.00f);
    c[ImGuiCol_TabHovered]      = ImVec4(0.18f, 0.40f, 0.55f, 1.00f);
    c[ImGuiCol_TabSelected]     = ImVec4(0.13f, 0.42f, 0.60f, 1.00f);
    c[ImGuiCol_Button]          = ImVec4(0.13f, 0.18f, 0.27f, 1.00f);
    c[ImGuiCol_ButtonHovered]   = ImVec4(0.18f, 0.40f, 0.55f, 1.00f);
    c[ImGuiCol_ButtonActive]    = ImVec4(0.20f, 0.52f, 0.72f, 1.00f);
    c[ImGuiCol_Header]          = ImVec4(0.13f, 0.18f, 0.27f, 1.00f);
    c[ImGuiCol_HeaderHovered]   = ImVec4(0.18f, 0.40f, 0.55f, 1.00f);
    c[ImGuiCol_HeaderActive]    = ImVec4(0.20f, 0.52f, 0.72f, 1.00f);
    c[ImGuiCol_CheckMark]       = accent;
    c[ImGuiCol_SliderGrab]      = accent;
    c[ImGuiCol_NavHighlight]    = accent;
    c[ImGuiCol_Separator]       = ImVec4(0.20f, 0.25f, 0.33f, 1.00f);
    c[ImGuiCol_Text]            = ImVec4(0.90f, 0.93f, 0.97f, 1.00f);
    c[ImGuiCol_TextDisabled]    = ImVec4(0.55f, 0.60f, 0.68f, 1.00f);
}

void save()
{
    if (port_settings_save(port_settings_get(), port_settings_path()))
        fprintf(stderr, "[ui] could not write %s\n", port_settings_path());
}

int lang_index(const char* pack)
{
    for (int i = 0; i < (int)(sizeof k_langs / sizeof k_langs[0]); i++)
        if (!strcmp(k_langs[i].pack, pack)) return i;
    return 0;   /* English */
}

/* A combo over the language list; only voice packs when `voice`. */
bool lang_combo(const char* label, char* pack, size_t cap, bool voice)
{
    const Lang& cur = k_langs[lang_index(pack)];
    bool changed = false;
    if (ImGui::BeginCombo(label, cur.name)) {
        for (const Lang& l : k_langs) {
            if (voice && !l.voice) continue;
            const bool sel = !strcmp(l.pack, pack);
            if (ImGui::Selectable(l.name, sel) && !sel) {
                snprintf(pack, cap, "%s", l.pack);
                changed = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

void hint(const char* t)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s", t);
    ImGui::PopStyleColor();
}

/* A row an environment variable decides (settings.h): shown, not editable. */
bool pinned(unsigned bit) { return (port_settings_pinned() & bit) != 0; }
void pin_begin(unsigned bit) { ImGui::BeginDisabled(pinned(bit)); }
void pin_end(unsigned bit)
{
    ImGui::EndDisabled();
    if (pinned(bit) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Set by an environment variable (play.sh or the terminal)");
}

/* `t` inside single quotes for the shell. */
std::string sh_quote(const std::string& t)
{
    std::string q = "'";
    for (char c : t) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return q + "'";
}

const char* saves_dir()
{
    const char* e = getenv("PS3_SAVEDATA_ROOT");
    return (e && *e) ? e : "gamedata/dev_hdd0/home/00000001/savedata";
}

#ifdef _WIN32
/* One argument onto a CreateProcess command line, quoted the way the CRT's
 * argv parser takes it apart again. */
void win_quote_arg(std::string& cmd, const char* a)
{
    if (!cmd.empty()) cmd += ' ';
    if (*a && !strpbrk(a, " \t\n\v\"")) { cmd += a; return; }
    cmd += '"';
    for (const char* p = a;; p++) {
        size_t bs = 0;
        while (*p == '\\') { p++; bs++; }
        if (!*p) { cmd.append(bs * 2, '\\'); break; }
        cmd.append(*p == '"' ? bs * 2 + 1 : bs, '\\');
        cmd += *p;
    }
    cmd += '"';
}

/* What execve does on Linux: the same binary, the same arguments, the new
 * environment. Windows has no exec, so a new process and this one exits.
 * Returns only on failure. */
void win_restart(char** argv, char** env)
{
    char self[4096];
    const DWORD n = GetModuleFileNameA(NULL, self, sizeof self);
    if (!n || n >= sizeof self) return;
    std::string cmd;
    for (char** a = argv; *a; a++) win_quote_arg(cmd, *a);
    std::string block;
    for (char** e = env; *e; e++) { block += *e; block += '\0'; }
    block += '\0';
    STARTUPINFOA si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(self, &cmd[0], NULL, NULL, TRUE, 0, &block[0], NULL, &si, &pi)) return;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    _Exit(0);
}
#endif

void open_folder(const char* path)
{
#ifdef _WIN32
    char full[MAX_PATH];
    if (!GetFullPathNameA(path, sizeof full, full, NULL)) snprintf(full, sizeof full, "%s", path);
    for (char* p = full; *p; p++) if (*p == '/') *p = '\\';
    if ((INT_PTR)ShellExecuteA(NULL, "open", full, NULL, NULL, SW_SHOWNORMAL) <= 32)
        fprintf(stderr, "[ui] could not open %s\n", path);
#else
    const std::string cmd = "xdg-open " + sh_quote(path) + " >/dev/null 2>&1 &";
    if (system(cmd.c_str()) != 0) fprintf(stderr, "[ui] could not open %s\n", path);
#endif
}

std::string g_last_backup;

bool backup_saves()
{
    struct stat st;
    if (stat(saves_dir(), &st) != 0) { g_last_backup = "No saves to back up"; return true; }
    char ts[32];
    const time_t t = time(nullptr);
    strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", localtime(&t));
    const char* u = getenv("PS3_USER_DIR");
    const std::string dst = std::string(u && *u ? u : "user") + "/backups/" + ts;
#ifdef _WIN32
    std::error_code ec;
    std::filesystem::create_directories(dst, ec);
    if (!ec) std::filesystem::copy(saves_dir(), dst, std::filesystem::copy_options::recursive, ec);
    const bool ok = !ec;
#else
    const std::string cmd = "mkdir -p " + sh_quote(dst) + " && cp -a " + sh_quote(std::string(saves_dir()) + "/.") +
                            " " + sh_quote(dst + "/");
    const bool ok = system(cmd.c_str()) == 0;
#endif
    g_last_backup = ok ? "Backed up to " + dst : "Backing up the saves failed";
    return ok;
}

void page_video(port_settings& s)
{
    int sc = s.scale - 1;
    pin_begin(PORT_PIN_SCALE);
    if (ImGui::Combo("Resolution", &sc, k_scales, 3)) { s.scale = sc + 1; save(); }
    pin_end(PORT_PIN_SCALE);
    /* The window as it is: F11 changes it without the menu. */
    bool b = rsx_vulkan_backend_fullscreen();
    if (ImGui::Checkbox("Fullscreen  (F11)", &b)) { s.fullscreen = b; rsx_vulkan_backend_set_fullscreen(b); save(); }
    b = s.vsync;
    if (ImGui::Checkbox("VSync", &b)) { s.vsync = b; rsx_vulkan_backend_set_vsync(b); save(); }
    b = s.stretch;
    if (ImGui::Checkbox("Stretch the picture (no black bars)", &b)) {
        s.stretch = b; rsx_vulkan_backend_set_stretch(b); save();
    }
    const int nd = SDL_GetNumVideoDisplays();
    if (nd > 1 && !pinned(PORT_PIN_DISPLAY)) {
        const char* cur = SDL_GetDisplayName(s.display < nd ? s.display : 0);
        if (ImGui::BeginCombo("Monitor", cur ? cur : "?")) {
            for (int i = 0; i < nd; i++) {
                const char* n = SDL_GetDisplayName(i);
                char label[160]; snprintf(label, sizeof label, "%d: %s", i + 1, n ? n : "?");
                if (ImGui::Selectable(label, i == s.display)) { s.display = i; save(); }
            }
            ImGui::EndCombo();
        }
    }
    hint("Resolution and monitor take effect the next time the game starts.");
}

void page_language(port_settings& s)
{
    pin_begin(PORT_PIN_VOICE);
    if (lang_combo("Voices", s.voice, sizeof s.voice, true)) save();
    pin_end(PORT_PIN_VOICE);
    pin_begin(PORT_PIN_TEXT);
    if (lang_combo("Subtitles and text", s.text, sizeof s.text, false)) save();
    pin_end(PORT_PIN_TEXT);
    hint("Voices and text can differ, for example English voices with French text. "
         "The language takes effect the next time the game starts.");
}

void set_hud(int mode)
{
    port_settings& s = *port_settings_get();
    s.fps_counter = mode >= 0 && mode <= 2 ? mode : 0;
    rsx_ui_set_visible(s.fps_counter || rsx_ui_menu_open());
    save();
}

void page_performance(port_settings& s)
{
    ImGui::TextUnformatted("Preset");
    const char* names[] = { "Performance (720p)", "Balanced (1440p)", "Quality (4K)" };
    pin_begin(PORT_PIN_SCALE | PORT_PIN_RECORD_THREAD | PORT_PIN_FPS_LIMIT);
    for (int i = 0; i < 3; i++) {
        if (i) ImGui::SameLine();
        if (ImGui::Button(names[i])) { s.scale = i + 1; s.record_thread = 1; s.fps_limit = 0;
                                       rsx_vulkan_backend_set_fps_limit(0); save(); }
    }
    pin_end(PORT_PIN_SCALE | PORT_PIN_RECORD_THREAD | PORT_PIN_FPS_LIMIT);
    int li = 0;
    for (int i = 0; i < 5; i++) if (k_fps_limits[i] == s.fps_limit) li = i;
    if (s.fps_limit > 0 && k_fps_limits[li] != s.fps_limit) li = 4;
    pin_begin(PORT_PIN_FPS_LIMIT);
    if (ImGui::Combo("FPS limit", &li, k_fps_names, 5)) {
        int v = k_fps_limits[li];
        if (v < 0) {
            SDL_DisplayMode dm;
            v = SDL_GetCurrentDisplayMode(rsx_ui_display_index(), &dm) == 0 && dm.refresh_rate > 0
                ? dm.refresh_rate : 60;
        }
        s.fps_limit = v; rsx_vulkan_backend_set_fps_limit(v); save();
    }
    pin_end(PORT_PIN_FPS_LIMIT);
    int fr = s.frame_rate == 30 ? 1 : 0;
    static const char* const k_fr[] = { "60 fps", "30 fps (as on the PS3)" };
    pin_begin(PORT_PIN_FRAME_RATE);
    if (ImGui::Combo("Frame rate", &fr, k_fr, 2)) {
        s.frame_rate = fr ? 30 : 60; port_sim_set_fps(s.frame_rate); save();
    }
    pin_end(PORT_PIN_FRAME_RATE);
    hint("The game's speed is right at either: 60 is smoother when the machine holds it; "
         "30 is steadier when it cannot (an uneven 40-55 fps judders on a 60 Hz screen).");
    static const char* const k_hud[] = { "Off", "Detailed", "Compact" };
    int h = s.fps_counter >= 0 && s.fps_counter <= 2 ? s.fps_counter : 0;
    if (ImGui::Combo("Performance HUD  (F10)", &h, k_hud, 3)) set_hud(h);
    bool b = s.record_thread;
    pin_begin(PORT_PIN_RECORD_THREAD);
    if (ImGui::Checkbox("Vulkan recording thread", &b)) { s.record_thread = b; save(); }
    pin_end(PORT_PIN_RECORD_THREAD);
    hint("The recording thread takes effect the next time the game starts.");
}

void page_mods(port_settings& s)
{
    ImGui::TextUnformatted("Beyond the PS3");
    bool b = s.fullres_particles;
    pin_begin(PORT_PIN_PARTICLES);
    if (ImGui::Checkbox("Full-resolution particles", &b)) { s.fullres_particles = b; save(); }
    pin_end(PORT_PIN_PARTICLES);
    hint("Particle effects the game draws at a reduced resolution and stretches; "
         "on, they are drawn at the resolution of the rest of the picture.");
    static const char* const k_aniso_names[] = { "Game default", "2x", "4x", "8x", "16x" };
    static const int k_aniso[] = { 0, 2, 4, 8, 16 };
    int ai = 0;
    for (int i = 0; i < 5; i++) if (k_aniso[i] == s.aniso) ai = i;
    pin_begin(PORT_PIN_ANISO);
    if (ImGui::Combo("Anisotropic filtering", &ai, k_aniso_names, 5)) { s.aniso = k_aniso[ai]; save(); }
    pin_end(PORT_PIN_ANISO);
    hint("Sharper ground and walls at a distance and at an angle.");

    ImGui::Separator();
    ImGui::TextUnformatted("Installed mods");
    static port_mod mods[64];
    static int nmods = -1;
    if (nmods < 0 || ImGui::Button("Rescan")) nmods = port_mods_scan(mods, 64);
    if (!nmods) {
        hint("No mods in the mods/ folder. Each mod is a folder with files/ mirroring the game's "
             "USRDIR and, optionally, a mod.ini (name, author, version, description).");
    }
    for (int i = 0; i < nmods; i++) {
        bool on = port_mod_enabled(&s, mods[i].dir);
        char label[200];
        snprintf(label, sizeof label, "%s%s%s##%s", mods[i].name, mods[i].version[0] ? "  v" : "",
                 mods[i].version, mods[i].dir);
        if (ImGui::Checkbox(label, &on)) { port_mod_set(&s, mods[i].dir, on); save(); }
        if (mods[i].description[0] || mods[i].author[0]) {
            char d[400];
            snprintf(d, sizeof d, "%s%s%s", mods[i].description, mods[i].author[0] ? "  -- " : "", mods[i].author);
            hint(d);
        }
    }
    hint("Mods enabled last win when two replace the same file. "
         "The options on this page take effect the next time the game starts.");
}

extern "C" int cellPad_port_of_instance(int instance_id);   /* libs/input/cellPad.c */
void page_controller(port_settings& s)
{
    /* Every game controller SDL sees, with the player port the game gave it.
     * One that is not open yet (the start screen, before the game has looked
     * for pads) is opened here only to read it, and closed again. On the
     * start screen nothing has started SDL's controller support yet. */
    if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0) {
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");   /* as cellPad sets it */
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    }
    int shown = 0;
    /* Under SDL's joystick lock, as rsx_ui.cpp: cellPad opens and polls the
     * same pads from the game's threads. */
    SDL_LockJoysticks();
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i)) continue;
        const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        SDL_GameController* gc = SDL_GameControllerFromInstanceID(id);
        const bool ours = gc == nullptr;
        if (ours) gc = SDL_GameControllerOpen(i);
        if (!gc) continue;
        const int port = cellPad_port_of_instance((int)id);
        const char* name = SDL_GameControllerName(gc);
        if (port >= 0) ImGui::Text("Player %d: %s", port + 1, name ? name : "controller");
        else           ImGui::Text("%s (%s)", name ? name : "controller",
                                   ours ? "the game picks it up when it starts" : "no free player slot: four are in use");
        static const struct { SDL_GameControllerButton b; const char* n; } k[] = {
            { SDL_CONTROLLER_BUTTON_A, "Cross/A" }, { SDL_CONTROLLER_BUTTON_B, "Circle/B" },
            { SDL_CONTROLLER_BUTTON_X, "Square/X" }, { SDL_CONTROLLER_BUTTON_Y, "Triangle/Y" },
            { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, "L1" }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, "R1" },
            { SDL_CONTROLLER_BUTTON_BACK, "Select" }, { SDL_CONTROLLER_BUTTON_START, "Start" },
            { SDL_CONTROLLER_BUTTON_LEFTSTICK, "L3" }, { SDL_CONTROLLER_BUTTON_RIGHTSTICK, "R3" },
            { SDL_CONTROLLER_BUTTON_DPAD_UP, "Up" }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, "Down" },
            { SDL_CONTROLLER_BUTTON_DPAD_LEFT, "Left" }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, "Right" },
        };
        std::string held;
        for (const auto& e : k)
            if (SDL_GameControllerGetButton(gc, e.b)) { held += held.empty() ? "" : "  "; held += e.n; }
        const int l2 = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) * 100 / 32767;
        const int r2 = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) * 100 / 32767;
        ImGui::Text("    Held: %s   L2 %d%%  R2 %d%% (R2 is the throttle)", held.empty() ? "none" : held.c_str(), l2, r2);
        if (ours) SDL_GameControllerClose(gc);
        shown++;
    }
    SDL_UnlockJoysticks();
    if (!shown)
        ImGui::Text("No controller found. Plug one in at any time: the game picks it up; up to four play split screen.");
    ImGui::Separator();
    if (ImGui::Button("Open the saves folder")) open_folder(saves_dir());
    ImGui::SameLine();
    if (ImGui::Button("Back up the saves")) backup_saves();
    if (ImGui::Button("Open the logs folder")) open_folder("work/runs");
    if (!g_last_backup.empty()) hint(g_last_backup.c_str());
    ImGui::Separator();
    bool b = s.skip_launcher;
    if (ImGui::Checkbox("Skip the start screen (F1 opens the options in game)", &b)) { s.skip_launcher = b; save(); }
}

/* The options' pages, as tabs: the in-game menu and the start screen's
 * Options page share them. */
void option_tabs(port_settings& s)
{
    if (ImGui::BeginTabItem("Video")) { page_video(s); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Language")) { page_language(s); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Performance")) { page_performance(s); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Mods")) { page_mods(s); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Controller & saves")) { page_controller(s); ImGui::EndTabItem(); }
}

/* What changed since the game started and needs a restart. */
std::string restart_note(const port_settings& s)
{
    std::string n;
    auto add = [&](const char* w) { n += n.empty() ? "" : ", "; n += w; };
    if (s.scale != g_boot.scale) add("resolution");
    if (s.display != g_boot.display) add("monitor");
    if (strcmp(s.voice, g_boot.voice) || strcmp(s.text, g_boot.text)) add("language");
    if (s.record_thread != g_boot.record_thread) add("recording thread");
    return n;
}

void draw_menu()
{
    port_settings& s = *port_settings_get();
    ImGuiIO& io = ImGui::GetIO();
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 150));
    const float fs = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(fs * 38.0f < io.DisplaySize.x * 0.92f ? fs * 38.0f : io.DisplaySize.x * 0.92f,
                                    fs * 26.0f < io.DisplaySize.y * 0.9f ? fs * 26.0f : io.DisplaySize.y * 0.9f),
                             ImGuiCond_Always);
    ImGui::Begin("MotorStorm Apocalypse  ·  Options", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    if (ImGui::BeginTabBar("pages")) {
        option_tabs(s);
        ImGui::EndTabBar();
    }
    const std::string note = restart_note(s);
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - fs * 3.2f);
    ImGui::Separator();
    if (!note.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.35f, 1.0f));
        ImGui::Text("Restart the game to apply: %s.", note.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }
    /* Right-aligned by its own width: a fixed 7.5 em left the button half
     * outside the window at 720p. */
    { const ImGuiStyle& st = ImGui::GetStyle();
      ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ImGui::CalcTextSize("Close  (F1)").x -
                           st.FramePadding.x * 2.0f - st.WindowPadding.x); }
    if (ImGui::Button("Close  (F1)")) rsx_ui_set_menu_open(0);
    ImGui::End();
}

/* ---- performance HUD (F10: detailed, compact, off) ------------------------
 * After the Pacific Rift port's. Frame times come from the guest's
 * presented-frame counter, sampled on each UI frame: the time since it last
 * moved, split over the frames it moved by. The last k_ft samples feed the
 * graph and the 1% low / worst figures. */
constexpr int k_ft = 240;
float g_ft[k_ft];               /* ms, ring */
int g_ft_n = 0, g_ft_i = 0;
double g_ft_since = 0.0;        /* s since the counter last moved */
unsigned g_ft_last = 0;
double g_cpu = -1.0;            /* this process, % of one core */

void hud_sample(double dt)
{
    const unsigned f = ppu_boot_frames_presented();
    g_ft_since += dt;
    if (f != g_ft_last) {
        const unsigned d = f - g_ft_last;
        g_ft_last = f;
        if (d < 1000) {
            const float ms = (float)(g_ft_since * 1000.0 / d);
            for (unsigned k = 0; k < d && k < (unsigned)k_ft; k++) {
                g_ft[g_ft_i] = ms; g_ft_i = (g_ft_i + 1) % k_ft;
                if (g_ft_n < k_ft) g_ft_n++;
            }
        }
        g_ft_since = 0.0;
    }
#ifndef _WIN32
    /* CPU time of the whole process (every guest thread) every half second. */
    static double acc = 0.0, last_cpu = -1.0;
    acc += dt;
    if (acc >= 0.5) {
        if (FILE* fp = fopen("/proc/self/stat", "r")) {
            char buf[1024];
            const size_t n = fread(buf, 1, sizeof buf - 1, fp);
            fclose(fp);
            buf[n] = 0;
            const char* q = strrchr(buf, ')');
            unsigned long ut = 0, st = 0;
            /* after the name: state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime */
            if (q && sscanf(q + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) == 2) {
                const double t = (double)(ut + st) / (double)sysconf(_SC_CLK_TCK);
                if (last_cpu >= 0.0) g_cpu = (t - last_cpu) / acc * 100.0;
                last_cpu = t;
            }
        }
        acc = 0.0;
    }
#endif
}

ImU32 fps_colour(double fps, double target)
{
    if (fps >= target * 0.95) return IM_COL32(120, 230, 120, 255);
    if (fps >= target * 0.75) return IM_COL32(255, 205, 90, 255);
    return IM_COL32(255, 100, 80, 255);
}

void draw_fps_compact()
{
    const ImGuiIO& io = ImGui::GetIO();
    char t[32];
    snprintf(t, sizeof t, "%.0f fps", g_fps);
    const ImVec2 sz = ImGui::CalcTextSize(t);
    const ImVec2 p(io.DisplaySize.x - sz.x - 18.0f, 12.0f);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(p.x - 8, p.y - 4), ImVec2(p.x + sz.x + 8, p.y + sz.y + 4), IM_COL32(0, 0, 0, 150), 6.0f);
    dl->AddText(p, fps_colour(g_fps, port_sim_fps()), t);
}

void draw_fps_detailed()
{
    const ImGuiIO& io = ImGui::GetIO();
    const float fs = ImGui::GetFontSize();
    const int target = port_sim_fps();
    const double target_ms = 1000.0 / target;

    /* Average, worst and 1% low over the samples held. */
    float sorted[k_ft];
    double sum = 0.0;
    for (int k = 0; k < g_ft_n; k++) { sorted[k] = g_ft[k]; sum += g_ft[k]; }
    std::sort(sorted, sorted + g_ft_n);
    const double avg = g_ft_n ? sum / g_ft_n : 0.0;
    const double worst = g_ft_n ? sorted[g_ft_n - 1] : 0.0;
    double low1 = 0.0;              /* mean fps of the slowest 1% of frames */
    if (g_ft_n) {
        const int m = g_ft_n / 100 > 0 ? g_ft_n / 100 : 1;
        double sm = 0.0;
        for (int k = g_ft_n - m; k < g_ft_n; k++) sm += sorted[k];
        low1 = 1000.0 / (sm / m);
    }
    /* Frame-to-frame swings over 4 ms: what reads as stutter. */
    int jumps = 0;
    for (int k = 1; k < g_ft_n; k++) {
        const float a = g_ft[(g_ft_i - g_ft_n + k - 1 + k_ft) % k_ft], b = g_ft[(g_ft_i - g_ft_n + k + k_ft) % k_ft];
        if (a - b > 4.0f || b - a > 4.0f) jumps++;
    }

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 12.0f, 12.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.62f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(fs * 0.7f, fs * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(fs * 0.5f, fs * 0.2f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::Begin("##hud", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    const float w = fs * 15.0f;

    ImGui::SetWindowFontScale(1.6f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(fps_colour(g_fps, target)));
    ImGui::Text("%.0f", g_fps);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("fps  / %d", target);

    ImGui::Text("%.1f ms avg   %.1f ms worst", avg, worst);
    ImGui::Text("1%% low %.0f fps   %d%% uneven", low1, g_ft_n > 1 ? jumps * 100 / (g_ft_n - 1) : 0);

    /* frame-time graph: bars, oldest left; dashed target line */
    const float gh = fs * 3.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + gh), IM_COL32(0, 0, 0, 90), 4.0f);
    const float top_ms = (float)(target_ms * 2.5);
    const float bw = w / k_ft;
    for (int k = 0; k < g_ft_n; k++) {
        const float ms = g_ft[(g_ft_i - g_ft_n + k + k_ft) % k_ft];
        const float hgt = gh * (ms < top_ms ? ms / top_ms : 1.0f);
        const float x = p0.x + (k_ft - g_ft_n + k) * bw;
        const ImU32 c = ms <= target_ms * 1.05 ? IM_COL32(120, 230, 120, 200)
                      : ms <= target_ms * 1.5 ? IM_COL32(255, 205, 90, 220) : IM_COL32(255, 100, 80, 240);
        dl->AddRectFilled(ImVec2(x, p0.y + gh - hgt), ImVec2(x + (bw > 1.5f ? bw - 0.5f : bw), p0.y + gh), c);
    }
    const float ty = p0.y + gh - gh * (float)(target_ms / top_ms);
    for (float x = p0.x; x < p0.x + w; x += 8.0f)
        dl->AddLine(ImVec2(x, ty), ImVec2(x + 4.0f < p0.x + w ? x + 4.0f : p0.x + w, ty), IM_COL32(255, 255, 255, 110));
    ImGui::Dummy(ImVec2(w, gh));

    ImGui::TextDisabled("%ux%u  (%dx)", 1280u * g_boot.scale, 720u * g_boot.scale, g_boot.scale);
    if (g_cpu >= 0.0) ImGui::TextDisabled("%u draws   CPU %.0f%%", rsx_vulkan_backend_guest_draws(), g_cpu);
    else              ImGui::TextDisabled("%u draws", rsx_vulkan_backend_guest_draws());

    ImGui::End();
    ImGui::PopStyleVar(3);
}

void draw_fps()
{
    if (port_settings_get()->fps_counter == 2) draw_fps_compact();
    else draw_fps_detailed();
}

/* The window's thread, under rsx_ui's lock: no rsx_ui_set_draw/_set_hotkey here. */
int hotkey(int key, void*)
{
    if (key != SDLK_F10 || g_start) return 0;
    static const int next[] = { 1, 2, 0 };         /* detailed -> compact -> off */
    set_hud(next[port_settings_get()->fps_counter % 3]);
    static const char* const n[] = { "off", "detailed", "compact" };
    fprintf(stderr, "[ui] HUD %s\n", n[port_settings_get()->fps_counter % 3]);
    return 1;
}

/* "Play": boot, or start the process again when an option that only takes
 * effect at start changed here. */
void play()
{
    port_settings& s = *port_settings_get();
    save();
    if (!restart_note(s).empty() && g_argv) {
        fprintf(stderr, "[ui] starting again with the new settings\n");
        fflush(stderr); fflush(stdout);
        if (char** env = port_settings_restart_env(nullptr)) {
            /* The binary's own path, not /proc/self/exe: the process keeps
             * its name (MotorStormRecomp), which play.sh and the scripts look for. */
#ifdef _WIN32
            win_restart(g_argv, env);
#else
            char self[4096];
            const ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
            if (n > 0) { self[n] = 0; execve(self, g_argv, env); }
            execve("/proc/self/exe", g_argv, env);
#endif
            free(env);
        }
        fprintf(stderr, "[ui] could not start again: the old settings stay for this run\n");
    }
    g_start = false;
    rsx_ui_set_modal(0);
    rsx_ui_set_menu_open(0);
    rsx_ui_set_visible(s.fps_counter);
    g_go = 1;
}

/* The start screen, over the disc's own picture (PIC1.PNG): Play, the port's
 * Options, Quit. MotorStorm has its own main menu and save handling, so Play
 * just boots the game. A pad moves through it as a keyboard or a mouse does. */
enum MenuPage { MENU_MAIN, MENU_OPTIONS };
MenuPage g_page = MENU_MAIN;
bool g_page_focus = true;      /* focus the page's first item on its first frame */

const ImU32 k_accent = IM_COL32(70, 200, 255, 255);

/* One line of the menu: big text, an optional line under it, a bar beside
 * the one under the pointer or the pad. */
bool menu_item(const char* id, const char* label, const char* sub, float scale = 1.0f)
{
    const float fs = ImGui::GetFontSize();
    ImFont* tf = (ImFont*)rsx_ui_title_font();
    const float big = fs * 1.75f * scale;
    const float h = big * 1.25f + (sub ? fs * 1.35f : 0.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (g_page_focus) { ImGui::SetKeyboardFocusHere(); g_page_focus = false; }
    const bool pressed = ImGui::Selectable(id, false, 0, ImVec2(ImGui::GetContentRegionAvail().x, h));
    const bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hot) dl->AddRectFilled(p, ImVec2(p.x + fs * 0.22f, p.y + h - fs * 0.25f), k_accent);
    dl->AddText(tf, big, ImVec2(p.x + fs * 0.85f, p.y), hot ? IM_COL32(255, 255, 255, 255)
                                                            : IM_COL32(190, 196, 206, 210), label);
    if (sub)
        dl->AddText(ImVec2(p.x + fs * 0.9f, p.y + big * 1.12f), hot ? IM_COL32(150, 215, 255, 255)
                                                                    : IM_COL32(150, 158, 170, 190), sub);
    return pressed;
}

/* PIC1.PNG filling the window, darkened on the left where the menu is. */
void draw_backdrop(const ImVec2& d)
{
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    int iw = 0, ih = 0;
    if (void* tex = rsx_ui_image(&iw, &ih)) {
        const float da = d.x / d.y, ia = (float)iw / (float)ih;
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (da > ia) { const float v = ia / da; uv0.y = (1 - v) * 0.5f; uv1.y = 1 - uv0.y; }
        else         { const float u = da / ia; uv0.x = (1 - u) * 0.5f; uv1.x = 1 - uv0.x; }
        bg->AddImage((ImTextureID)(uintptr_t)tex, ImVec2(0, 0), d, uv0, uv1);
    } else {
        bg->AddRectFilledMultiColor(ImVec2(0, 0), d, IM_COL32(14, 22, 38, 255), IM_COL32(8, 12, 22, 255),
                                    IM_COL32(2, 3, 6, 255), IM_COL32(4, 6, 12, 255));
    }
    const ImU32 dark = IM_COL32(0, 0, 0, 225), clear = IM_COL32(0, 0, 0, 0);
    bg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(d.x * 0.65f, d.y), dark, clear, clear, dark);
    bg->AddRectFilled(ImVec2(0, 0), d, IM_COL32(0, 0, 0, g_page == MENU_OPTIONS ? 120 : 40));
}

void go_page(MenuPage p) { g_page = p; g_page_focus = true; }

void draw_start()
{
    port_settings& s = *port_settings_get();
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 d = io.DisplaySize;
    const float fs = ImGui::GetFontSize();
    draw_backdrop(d);

    /* the title, top left */
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    ImFont* tf = (ImFont*)rsx_ui_title_font();
    const ImVec2 tp(d.x * 0.07f, d.y * 0.10f);
    const float th = fs * 3.4f;
    bg->AddText(tf, th, tp, IM_COL32(255, 255, 255, 255), "MotorStorm Apocalypse");
    bg->AddRectFilled(ImVec2(tp.x + 4, tp.y + th * 1.06f), ImVec2(tp.x + 4 + fs * 5.0f, tp.y + th * 1.06f + 5), k_accent);

    /* Circle, Escape: back to the main page. */
    if (g_page != MENU_MAIN &&
        (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
        go_page(MENU_MAIN);

    const float top = tp.y + th * 1.06f + fs * 2.4f;
    const float width = g_page == MENU_OPTIONS ? (fs * 38.0f < d.x * 0.86f ? fs * 38.0f : d.x * 0.86f)
                                               : (fs * 34.0f < d.x * 0.62f ? fs * 34.0f : d.x * 0.62f);
    const float height = d.y - top - fs * 3.2f;
    ImGui::SetNextWindowPos(ImVec2(tp.x - fs * 0.9f, top), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
    if (g_page != MENU_OPTIONS) ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_NavHighlight, ImVec4(0, 0, 0, 0));
    ImGui::Begin("##start", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoScrollbar);
    bool go = false;
    switch (g_page) {
    case MENU_MAIN: {
        if (menu_item("##play", "PLAY", nullptr)) go = true;
        if (menu_item("##options", "OPTIONS", nullptr)) go_page(MENU_OPTIONS);
        if (menu_item("##quit", "QUIT", nullptr)) {
            port_log_drain();
            _Exit(0);
        }
        break; }
    case MENU_OPTIONS: {
        ImGui::PopStyleColor(4);   /* the options' own widgets keep their colours */
        if (ImGui::BeginTabBar("pages")) {
            option_tabs(s);
            ImGui::EndTabBar();
        }
        const std::string note = restart_note(s);
        /* Room for the hint beside BACK to wrap onto a second line: at 3 em
         * from the bottom its second line was cut off. */
        ImGui::SetCursorPosY(height - fs * 3.9f);
        ImGui::Separator();
        if (g_page_focus) { ImGui::SetKeyboardFocusHere(); g_page_focus = false; }
        if (ImGui::Button("BACK", ImVec2(fs * 8.0f, fs * 1.8f))) go_page(MENU_MAIN);
        ImGui::SameLine();
        hint(note.empty() ? "Options are saved. F1 (or Select+Start held) opens them in game."
                          : ("The game restarts on Play to apply: " + note + ".").c_str());
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));   /* rebalance the pop above */
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_NavHighlight, ImVec4(0, 0, 0, 0));
        break; }
    }
    ImGui::End();
    ImGui::PopStyleColor(4);

    /* the controls, bottom left */
    const char* keys = g_page == MENU_MAIN ? "X / Enter  select" : "X / Enter  select      O / Esc  back";
    bg->AddText(ImVec2(tp.x, d.y - fs * 2.2f), IM_COL32(200, 205, 214, 200), keys);
    /* PS3_UI_AUTOPLAY_MS=<ms>: press Play by itself that long after the start
     * screen first draws -- a scripted run through the start screen, the way
     * work/play.sh boots, without a hand on the pad. */
    static long autoplay_ms = -2;
    static double autoplay_at = 0;
    if (autoplay_ms == -2) {
        const char* e = getenv("PS3_UI_AUTOPLAY_MS");
        autoplay_ms = (e && *e) ? atol(e) : -1;
        autoplay_at = ImGui::GetTime() + (double)autoplay_ms / 1000.0;
    }
    if (autoplay_ms >= 0 && g_page == MENU_MAIN && ImGui::GetTime() >= autoplay_at) {
        autoplay_ms = -1;
        go = true;
    }
    if (go) play();
}

void draw(void*)
{
    if (!g_styled) { style(); g_styled = true; }
    /* Frames presented over half a second; a fresh count after the UI was
     * hidden (no ImGui frames were built meanwhile). */
    static int last_frame = -10;
    if (ImGui::GetFrameCount() != last_frame + 1) {
        g_fps_frames = ppu_boot_frames_presented(); g_fps_t = 0.0;
        g_ft_last = g_fps_frames; g_ft_since = 0.0;
    }
    last_frame = ImGui::GetFrameCount();
    hud_sample(rsx_ui_frame_dt());
    g_fps_t += rsx_ui_frame_dt();
    if (g_fps_t >= 0.5) {
        const unsigned f = ppu_boot_frames_presented();
        g_fps = (double)(f - g_fps_frames) / g_fps_t;
        g_fps_frames = f; g_fps_t = 0.0;
    }
    if (g_start) { draw_start(); return; }
    if (port_settings_get()->fps_counter) draw_fps();
    if (rsx_ui_menu_open()) draw_menu();
}

} /* namespace */

/* The disc's PIC1.PNG (PS3_GAME, beside USRDIR) for the start screen. */
static void load_backdrop(const char* eboot)
{
#ifdef PORT_HAVE_PNG
    std::string dir = eboot ? eboot : "";
    for (int up = 0; up < 2; up++) {
        const size_t k = dir.find_last_of('/');
        dir = k == std::string::npos ? std::string(".") : dir.substr(0, k);
    }
    const std::string path = dir + "/PIC1.PNG";
    png_image im;
    memset(&im, 0, sizeof im);
    im.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&im, path.c_str())) {
        fprintf(stderr, "[ui] no picture at %s: the start screen has a plain backdrop\n", path.c_str());
        return;
    }
    im.format = PNG_FORMAT_RGBA;
    std::vector<unsigned char> px(PNG_IMAGE_SIZE(im));
    if (png_image_finish_read(&im, nullptr, px.data(), 0, nullptr))
        rsx_ui_set_image(px.data(), (int)im.width, (int)im.height);
    png_image_free(&im);
#else
    (void)eboot;
#endif
}

void port_ui_init(int argc, char** argv)
{
    g_boot = *port_settings_get();
    g_argc = argc; g_argv = argv;
    rsx_ui_set_draw(draw, nullptr);
    rsx_ui_set_hotkey(hotkey, nullptr);
    rsx_ui_set_visible(port_settings_get()->fps_counter);
    bool play_arg = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--play")) play_arg = true;
    }
    const char* skip = getenv("PS3_SKIP_LAUNCHER");
    g_want_start = !play_arg && !(skip && skip[0] == '1') && !port_settings_get()->skip_launcher;
    if (g_want_start) load_backdrop(argc > 1 ? argv[1] : nullptr);
    /* PS3_UI_PAGE=options: open the start screen on that page (for a
     * screenshot of it). */
    if (const char* pg = getenv("PS3_UI_PAGE")) {
        if (!strcmp(pg, "options")) g_page = MENU_OPTIONS;
    }
    if (g_want_start) g_go = 0;            /* port_ui_backend_ready decides */
}

void port_ui_backend_ready(void)
{
    if (!g_want_start) return;
    if (!rsx_ui_available()) {
        fprintf(stderr, "[ui] no UI: the game starts without its start screen\n");
        g_go = 1;
        return;
    }
    g_start = true;
    rsx_ui_set_modal(1);
    rsx_ui_set_visible(1);
}

void port_ui_wait_start(void)
{
    while (!g_go.load()) {
        /* The window went away under the start screen: nothing can press Play. */
        if (g_start.load() && !rsx_ui_available()) {
            fprintf(stderr, "[ui] the start screen is gone: the game starts\n");
            g_start = false;
            g_go = 1;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

int port_ui_start_screen(void) { return g_start.load() ? 1 : 0; }

#else

void port_ui_init(int, char**) {}
void port_ui_backend_ready(void) {}
void port_ui_wait_start(void) {}
int port_ui_start_screen(void) { return 0; }

#endif
