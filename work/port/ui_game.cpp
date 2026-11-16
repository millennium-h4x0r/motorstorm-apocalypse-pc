/* MotorStorm Apocalypse port, UI: where the game is, its set-up and checks
 * (ui_internal.h). One page for two places: the whole start screen while the
 * game is not ready (main.cpp boots nothing then), and a page of the menus
 * afterwards. What it shows comes from gamefiles.h; it changes one setting,
 * [game] path, and a new folder takes effect by starting the process again. */
#ifdef PS3RECOMP_UI

#include "ui_internal.h"
#include "gamefiles.h"
#include "ui.h"

#include "imgui.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace pui {

namespace {

/* The folder being looked at: the one this run started with, until another is
 * chosen here. */
bool g_init = false;
std::string g_path;
gamefiles::Report g_rep;
char g_typed[1024];
std::string g_note;                 /* the last thing a button did, one line */

const ImVec4 k_ok(0.47f, 0.90f, 0.47f, 1.0f), k_warn(1.0f, 0.80f, 0.35f, 1.0f), k_bad(1.0f, 0.45f, 0.38f, 1.0f);

void look(const std::string& path)
{
    g_path = path;
    g_rep = gamefiles::check(path, gamefiles::where_from_env());
}

void init()
{
    if (g_init) return;
    g_init = true;
    const gamefiles::Boot& b = gamefiles::boot();
    g_path = b.given;
    g_rep = b.report;
}

/* A folder the player chose or typed: look at it, and keep it in the settings
 * when it is a game's folder (even with its EBOOT.elf still to come). */
void choose(port_settings& s, const std::string& raw)
{
    std::string p = gamefiles::clean_path(raw);
    if (p.empty()) { g_note = "No folder was given."; return; }
    std::error_code ec;
    const std::filesystem::path abs = std::filesystem::absolute(std::filesystem::path(p), ec);
    if (!ec) p = abs.lexically_normal().string();
    p = gamefiles::clean_path(p);
    look(p);
    const gamefiles::Item* folder = g_rep.find("folder");
    if (!folder || folder->state != gamefiles::FOUND) {
        g_note = "That is not a game's folder: nothing was changed.";
        return;
    }
    if ((size_t)snprintf(s.game_path, sizeof s.game_path, "%s", g_rep.root.c_str()) >= sizeof s.game_path) {
        s.game_path[0] = 0;
        g_note = "That path is too long to keep.";
        return;
    }
    save();
    g_note = g_rep.ready ? "" : "The folder is kept. Something is still missing: see below.";
}

/* ---- the system's folder window, on a thread of its own ---------------------- */
#ifndef _WIN32
struct Pick {
    std::atomic<int> state{0};      /* 1 open, 2 closed: `result` is set */
    std::string result;             /* empty: cancelled */
};
std::shared_ptr<Pick> g_pick;

bool on_path(const char* tool)
{
    const char* path = getenv("PATH");
    if (!path) return false;
    std::string dir;
    for (const char* p = path;; p++) {
        if (*p && *p != ':') { dir += *p; continue; }
        if (!dir.empty() && access((dir + "/" + tool).c_str(), X_OK) == 0) return true;
        dir.clear();
        if (!*p) return false;
    }
}

/* kdialog on a KDE desktop, else zenity, else whichever is installed; NULL: neither. */
const char* pick_tool()
{
    static int known = -1;
    static const char* tool = nullptr;
    if (known < 0) {
        const bool k = on_path("kdialog"), z = on_path("zenity");
        const char* desk = getenv("XDG_CURRENT_DESKTOP");
        const bool kde = desk && strstr(desk, "KDE");
        tool = (k && (kde || !z)) ? "kdialog" : z ? "zenity" : nullptr;
        known = 1;
    }
    return tool;
}

void pick_open(const std::string& start)
{
    const char* tool = pick_tool();
    if (!tool || (g_pick && g_pick->state == 1)) return;
    const char* const title = "The folder of your MotorStorm Apocalypse dump (the one holding PS3_GAME)";
    std::string from = start;
    std::error_code ec;
    if (from.empty() || !std::filesystem::is_directory(from, ec)) from = getenv("HOME") ? getenv("HOME") : "/";
    std::string cmd;
    if (!strcmp(tool, "kdialog"))
        cmd = "kdialog --title " + sh_quote(title) + " --getexistingdirectory " + sh_quote(from);
    else
        cmd = "zenity --file-selection --directory --title=" + sh_quote(title) + " --filename=" + sh_quote(from + "/");
    cmd += " 2>/dev/null";
    auto p = std::make_shared<Pick>();
    p->state = 1;
    g_pick = p;
    /* The window stays open as long as the player likes: not on the UI's thread. */
    std::thread([p, cmd]() {
        std::string out;
        if (FILE* f = popen(cmd.c_str(), "r")) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
            if (pclose(f) != 0) out.clear();        /* cancelled, or it could not open */
        }
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        p->result = out;
        p->state = 2;
    }).detach();
}
#endif

void state_word(gamefiles::State st)
{
    switch (st) {
    case gamefiles::FOUND:   ImGui::TextColored(k_ok, "found"); break;
    case gamefiles::WARNING: ImGui::TextColored(k_warn, "note"); break;
    case gamefiles::MISSING: ImGui::TextColored(k_bad, "missing"); break;
    case gamefiles::WRONG:   ImGui::TextColored(k_bad, "not the right one"); break;
    }
}

/* One line of the list: a mark, the name, the state in a word; under it what
 * was found and, when something is to be done, what. */
void item_row(const gamefiles::Item& it, const char* number, bool told_folder)
{
    const float fs = ImGui::GetFontSize();
    const ImVec4& col = it.state == gamefiles::FOUND ? k_ok : it.state == gamefiles::WARNING ? k_warn : k_bad;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + fs * 0.45f, p.y + fs * 0.55f);
    dl->AddCircleFilled(c, fs * 0.36f, ImGui::ColorConvertFloat4ToU32(col));
    if (it.state == gamefiles::FOUND) {             /* a tick */
        const ImU32 dark = IM_COL32(10, 30, 12, 255);
        dl->AddLine(ImVec2(c.x - fs * 0.18f, c.y), ImVec2(c.x - fs * 0.04f, c.y + fs * 0.15f), dark, fs * 0.10f);
        dl->AddLine(ImVec2(c.x - fs * 0.04f, c.y + fs * 0.15f), ImVec2(c.x + fs * 0.19f, c.y - fs * 0.15f), dark, fs * 0.10f);
    } else {                                        /* an exclamation mark */
        const ImU32 dark = IM_COL32(40, 14, 8, 255);
        dl->AddLine(ImVec2(c.x, c.y - fs * 0.20f), ImVec2(c.x, c.y + fs * 0.05f), dark, fs * 0.10f);
        dl->AddCircleFilled(ImVec2(c.x, c.y + fs * 0.20f), fs * 0.055f, dark);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(fs * 0.45f, fs * 0.15f));
    ImGui::Indent(fs * 1.25f);
    if (number) ImGui::Text("%s  %s", number, it.name.c_str());
    else ImGui::TextUnformatted(it.name.c_str());
    ImGui::SameLine();
    /* Something that only waits for the folder is not a second problem. */
    const bool waits = told_folder && it.id != "folder" && it.state == gamefiles::MISSING;
    if (waits) ImGui::TextDisabled("needs the folder first");
    else state_word(it.state);
    if (!waits) {
        if (it.state == gamefiles::FOUND || it.state == gamefiles::WARNING) {
            /* On the same line when there is room for it. */
            ImGui::SameLine();
            if (ImGui::CalcTextSize(it.detail.c_str()).x > ImGui::GetContentRegionAvail().x) ImGui::NewLine();
            hint(it.detail.c_str());
        } else ImGui::TextWrapped("%s", it.detail.c_str());
        if (!it.help.empty()) hint(it.help.c_str());
    }
    ImGui::Unindent(fs * 1.25f);
    ImGui::Dummy(ImVec2(0.0f, number ? fs * 0.3f : fs * 0.1f));
    ImGui::PopStyleVar();
}

} /* namespace */

bool game_ready() { return gamefiles::boot().report.ready; }

const char* game_problem()
{
    static std::string line;
    const gamefiles::Report& r = gamefiles::boot().report;
    line = r.ready ? std::string() : "Set up: " + r.problem;
    return line.c_str();
}

void page_game(port_settings& s)
{
    init();
    const gamefiles::Boot& boot = gamefiles::boot();
    const float fs = ImGui::GetFontSize();
    const bool at_start = port_ui_start_screen() != 0;      /* nothing of the game runs yet */
    const bool env_pin = pinned(PORT_PIN_GAME);

#ifndef _WIN32
    if (g_pick && g_pick->state == 2) {
        const std::string got = g_pick->result;
        g_pick.reset();
        if (!got.empty()) choose(s, got);
    }
    const bool picking = g_pick && g_pick->state == 1;
#else
    const bool picking = false;
#endif

    /* PS3_UI_GAME_PATH=<path>: that path as if typed here and "Use this path"
     * pressed, once, on the page's first frame; with PS3_UI_GAME_GO=1 also the
     * button that follows when everything is found -- a scripted set-up, the
     * way PS3_UI_AUTOPLAY_MS presses Play. */
    static int scripted = -1;
    bool scripted_go = false;
    if (scripted < 0) {
        scripted = 0;
        const char* e = getenv("PS3_UI_GAME_PATH");
        if (e && *e && at_start && !env_pin) { choose(s, e); scripted = 1; }
    }
    if (scripted == 1) {
        scripted = 0;
        const char* g = getenv("PS3_UI_GAME_GO");
        scripted_go = g && g[0] == '1';
    }

    /* what this is about, in two lines */
    ImGui::TextUnformatted(boot.report.ready ? "Your game" : "Set up: your game");
    hint("The port plays your own copy of MotorStorm Apocalypse (BCES-00484, the European disc). It needs: "
         "1. the folder of the dumped disc, and 2. in it EBOOT.elf, the game's program unlocked with RPCS3.");
    ImGui::Separator();

    /* the result, and the way on */
    static bool go_shown = false;
    const bool go_now = g_rep.ready && (g_rep.root != boot.report.root || !boot.report.ready) && at_start;
    const bool other = g_rep.root != boot.report.root;       /* another folder than this run's */
    if (g_rep.ready && (other || !boot.report.ready)) {
        ImGui::TextColored(k_ok, "Everything the game needs is in this folder.");
        if (at_start) {
            /* The way on, under the pad or the keys as soon as it is there. */
            if (!go_shown) ImGui::SetKeyboardFocusHere();
            const bool go = ImGui::Button(boot.report.ready ? "Restart with this folder" : "Continue",
                                          ImVec2(fs * 14.0f, fs * 1.9f));
            if (!boot.report.ready) ImGui::SetItemDefaultFocus();
            ImGui::SameLine();
            hint("The port starts again with it.");
            if (go || scripted_go) {
                restart_for_game(true);
                g_note = "The port could not start itself again: close it and start it once more.";
            }
        }
    } else if (g_rep.ready) {
        ImGui::TextColored(k_ok, "Everything the game needs is here.");
    } else {
        ImGui::TextColored(k_bad, "%s.", g_rep.problem.c_str());
    }
    go_shown = go_now;
    if (!g_note.empty()) ImGui::TextColored(k_warn, "%s", g_note.c_str());

    /* 1. the folder, and choosing it */
    const gamefiles::Item* folder = g_rep.find("folder");
    const gamefiles::Item* eboot = g_rep.find("eboot");
    const bool no_folder = folder && folder->state != gamefiles::FOUND;
    if (folder) item_row(*folder, "1.", false);
    ImGui::Indent(fs * 1.25f);
    if (!at_start) {
        hint("The folder can be changed on the start screen, before the game runs.");
    } else if (env_pin) {
        hint("PS3_GAME_DIR decides the folder: change or unset it where the port is started.");
    } else {
#ifndef _WIN32
        if (pick_tool()) {
            ImGui::BeginDisabled(picking);
            if (ImGui::Button(picking ? "Waiting for the folder window..." : "Choose the game's folder...")) {
                g_note.clear();
                pick_open(g_rep.root);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
        }
#endif
        const char* const use = "Use this path";
        const ImGuiStyle& st = ImGui::GetStyle();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(use).x -
                                st.FramePadding.x * 2.0f - st.ItemSpacing.x);
        bool typed = ImGui::InputTextWithHint("##path", "or type or paste its path here", g_typed,
                                              sizeof g_typed, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        typed |= ImGui::Button(use);
        if (typed) { g_note.clear(); choose(s, g_typed); if (g_note.empty()) g_typed[0] = 0; }
        std::error_code ec;
        const bool can_default = s.game_path[0] && std::filesystem::is_directory("disc", ec);
        if (can_default) {
            if (ImGui::Button("Use the default folder (disc)")) {
                s.game_path[0] = 0;
                save();
                look("disc");
                g_note = "The folder chosen here is forgotten.";
            }
            ImGui::SameLine();
        }
        if (!other && boot.report.ready) {
            static const char* const k_from[] = {
                "This folder was given when the port was started (the command line).",
                "",
                "This folder was chosen here.",
                "This is the default: the disc folder beside the port.",
            };
            hint(k_from[boot.source]);
        } else if (can_default) ImGui::NewLine();
    }
    ImGui::Unindent(fs * 1.25f);

    /* 2. EBOOT.elf */
    if (eboot) {
        item_row(*eboot, "2.", no_folder);
        if (!no_folder && eboot->state != gamefiles::FOUND) {
            ImGui::Indent(fs * 1.25f);
            if (ImGui::Button("Show EBOOT.BIN")) open_folder(g_rep.usrdir.c_str());
            ImGui::SameLine();
            if (ImGui::Button("Check again")) { look(g_path); g_note = g_rep.ready ? "" : "Checked: not there yet."; }
            ImGui::SameLine();
            hint("Show EBOOT.BIN opens the game's USRDIR folder.");
            ImGui::Unindent(fs * 1.25f);
        }
    }

    /* the rest */
    ImGui::TextDisabled("Also checked");
    if (no_folder || !eboot || eboot->state == gamefiles::FOUND) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Check again")) { look(g_path); g_note = g_rep.ready ? "" : "Checked."; }
    }
    for (const gamefiles::Item& it : g_rep.items) {
        if (it.id == "folder" || it.id == "eboot") continue;
        /* Without a folder, what is in it is not a list of further problems. */
        if (no_folder && it.id != "firmware" && it.id != "space") continue;
        item_row(it, nullptr, no_folder);
    }

    ImGui::Separator();
    hint("The game's code is built into this port, made from that one EBOOT.elf; the folder only says where "
         "the game's data is. A disc image (.iso) is not read: unpack it to a folder first.");
}

} /* namespace pui */

#endif
