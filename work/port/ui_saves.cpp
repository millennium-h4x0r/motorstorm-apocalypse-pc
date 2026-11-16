/* MotorStorm Apocalypse port, UI: the Saves page (ui_internal.h) over saves.h:
 * the saves, back up now, the backups with Restore / Export / Delete, import
 * of a .zip, the folders, and the two options. Everything runs on the
 * window's thread as it is asked for: the saves are about 100 KB, a backup
 * or an export takes a millisecond or two, and an import is capped
 * (saves::k_max_archive). The lists are read again after each action and
 * every two seconds, so a .zip dropped into the exports folder shows up. */
#ifdef PS3RECOMP_UI

#include "ui_internal.h"
#include "saves.h"
#include "ui.h"

#include "imgui.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace pui {
namespace {

saves::Config g_cfg;
std::vector<saves::SaveInfo> g_saves;
std::vector<saves::BackupInfo> g_backups;
std::vector<std::string> g_archives;
double g_scanned = -1.0;
int g_last_frame = -10;

std::string g_note;             /* what the last action did */
bool g_note_ok = true;

enum Confirm { CONFIRM_NONE, CONFIRM_RESTORE, CONFIRM_DELETE };
Confirm g_confirm = CONFIRM_NONE;
saves::BackupInfo g_target;     /* the row the question is about */
bool g_ask = false;             /* open the question this frame */
char g_path[512];               /* a .zip's path, typed */

const char* const k_ask_restore = "Restore the saves?";
const char* const k_ask_delete = "Delete the backup?";

void rescan()
{
    g_saves = saves::list_saves(g_cfg.save_root);
    g_backups = saves::list_backups(g_cfg);
    g_archives = saves::list_archives(g_cfg);
    g_scanned = ImGui::GetTime();
}

void done(const saves::Result& r)
{
    g_note = r.message;
    g_note_ok = r.ok;
    if (!r.ok) fprintf(stderr, "[ui] saves: %s\n", r.message.c_str());
    rescan();
}

/* An action on the files; nothing it throws reaches the frame. */
template <class F> void run(F f)
{
    try { done(f()); }
    catch (const std::exception& e) {
        saves::Result r;
        r.message = std::string("That did not work: ") + e.what();
        done(r);
    }
}

std::string count(int n, const char* one, const char* many)
{
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

std::string backup_line(const saves::BackupInfo& b)
{
    return saves::format_time(b.time) + " \xC2\xB7 " + saves::reason_label(b.reason) + " \xC2\xB7 " +
           count(b.saves, "save", "saves") + " \xC2\xB7 " + saves::format_size(b.size);
}

std::string file_name(const std::string& path)
{
    const size_t k = path.find_last_of("/\\");
    return k == std::string::npos ? path : path.substr(k + 1);
}

/* The question before a restore or a delete. Cancel has the focus; Circle
 * and Escape cancel. */
void confirm_popup(bool running)
{
    if (g_ask) {
        /* The page behind a question goes dark, not ImGui's light grey. */
        ImGui::GetStyle().Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.60f);
        ImGui::OpenPopup(g_confirm == CONFIRM_RESTORE ? k_ask_restore : k_ask_delete);
    }
    const float fs = ImGui::GetFontSize();
    const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
    for (int which = CONFIRM_RESTORE; which <= CONFIRM_DELETE; which++) {
        ImGui::SetNextWindowPos(centre, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal(which == CONFIRM_RESTORE ? k_ask_restore : k_ask_delete, nullptr, flags)) continue;
        bool act = false, close = false;
        ImGui::PushTextWrapPos(fs * 26.0f);
        const std::string when = saves::format_time(g_target.time) + " (" + saves::reason_label(g_target.reason) + ", " +
                                 count(g_target.saves, "save", "saves") + ")";
        if (which == CONFIRM_RESTORE) {
            ImGui::TextWrapped("Restore the saves from %s?", when.c_str());
            ImGui::TextWrapped(g_saves.empty() ? "There are no saves now, so nothing is replaced."
                                               : "Your current saves are backed up first, then replaced.");
        } else {
            ImGui::TextWrapped("Delete the backup from %s?", when.c_str());
            ImGui::TextWrapped("The backup is removed from the disk. Your current saves are not touched.");
        }
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(fs * 26.0f, fs * 0.2f));
        const ImVec2 size(fs * 8.0f, fs * 1.8f);
        if (ImGui::Button(which == CONFIRM_RESTORE ? "Restore" : "Delete", size)) act = true;
        ImGui::SameLine();
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        if (ImGui::Button("Cancel", size)) close = true;
        ImGui::SetItemDefaultFocus();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) close = true;
        if (act) {
            const std::string name = g_target.name;
            if (which == CONFIRM_RESTORE) run([&] { return saves::restore(g_cfg, name, running); });
            else run([&] { return saves::remove_backup(g_cfg, name); });
            close = true;
        }
        if (close) { g_confirm = CONFIRM_NONE; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    g_ask = false;
}

} /* namespace */

void backup_before_play()
{
    const port_settings& s = *port_settings_get();
    if (!s.backup_on_play) return;
    try {
        const saves::Result r = saves::backup_for_play(saves::default_config(), s.keep_backups);
        fprintf(stderr, "[ui] saves before Play: %s\n", r.message.c_str());
        g_note = r.ok ? (r.skipped ? std::string() : "Play backed up the saves.") : "Play could not back up the saves. " + r.message;
        g_note_ok = r.ok;
    } catch (const std::exception& e) {
        fprintf(stderr, "[ui] saves before Play: %s\n", e.what());
    }
}

bool saves_popup_open() { return g_confirm != CONFIRM_NONE; }

void page_saves(port_settings& s)
{
    const float fs = ImGui::GetFontSize();
    /* PS3_UI_SAVES_RUNNING=1: the page as the in-game menu shows it, on the
     * start screen (for a screenshot of it). */
    static const bool as_running = [] { const char* e = getenv("PS3_UI_SAVES_RUNNING"); return e && e[0] == '1'; }();
    const bool running = !port_ui_start_screen() || as_running;
    const int frame = ImGui::GetFrameCount();
    if (frame != g_last_frame + 1) {        /* the page was away: start clean */
        g_cfg = saves::default_config();
        g_confirm = CONFIRM_NONE;
        g_ask = false;
        g_scanned = -1.0;
    }
    g_last_frame = frame;
    if (g_scanned < 0.0 || ImGui::GetTime() - g_scanned > 2.0) rescan();

    /* ---- the saves ---- */
    ImGui::TextUnformatted("Saves");
    if (g_saves.empty()) hint("No saves yet. The game makes one the first time it autosaves.");
    for (const saves::SaveInfo& v : g_saves) {
        ImGui::Bullet();
        ImGui::TextWrapped("%s", v.title.c_str());
        if (!v.detail.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", v.detail.c_str());
        ImGui::Indent();
        hint(((v.subtitle.empty() ? std::string() : v.subtitle + " \xC2\xB7 ") + v.dir + " \xC2\xB7 " +
              saves::format_size(v.size) + " \xC2\xB7 last written " + saves::format_time(v.mtime)).c_str());
        ImGui::Unindent();
    }
    ImGui::BeginDisabled(g_saves.empty());
    if (ImGui::Button("Back up now")) run([] { return saves::backup(g_cfg, "manual"); });
    ImGui::SameLine();
    if (ImGui::Button("Export to a .zip")) run([] { return saves::export_archive(g_cfg, ""); });
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Open the saves folder")) open_folder(g_cfg.save_root.c_str());
    if (!g_note.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, g_note_ok ? ImVec4(0.55f, 0.90f, 0.60f, 1.0f) : ImVec4(1.0f, 0.62f, 0.40f, 1.0f));
        ImGui::TextWrapped("%s", g_note.c_str());
        ImGui::PopStyleColor();
    }

    /* ---- the backups ---- */
    ImGui::Separator();
    ImGui::Text("Backups (%d)", (int)g_backups.size());
    if (g_backups.empty()) hint("No backups yet.");
    if (running && !g_backups.empty()) hint("Restore from the start screen (the game has the saves open).");
    std::string do_export, do_import;
    if (!g_backups.empty() && ImGui::BeginTable("backups", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("do", ImGuiTableColumnFlags_WidthFixed);
        for (const saves::BackupInfo& b : g_backups) {
            ImGui::PushID(b.name.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextWrapped("%s", backup_line(b).c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", b.name.c_str());
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(running || b.saves == 0);
            if (ImGui::Button("Restore")) { g_confirm = CONFIRM_RESTORE; g_target = b; g_ask = true; }
            ImGui::EndDisabled();
            if (running && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Restore from the start screen (the game has the saves open)");
            ImGui::SameLine();
            ImGui::BeginDisabled(b.saves == 0);
            if (ImGui::Button("Export")) do_export = b.name;
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Delete")) { g_confirm = CONFIRM_DELETE; g_target = b; g_ask = true; }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    /* After the loop: an action reads the lists again. */
    if (!do_export.empty()) run([&] { return saves::export_archive(g_cfg, do_export); });
    if (ImGui::Button("Open the backups folder")) {
        /* Made on the first backup; made here so that it opens. */
        saves::ensure_folder(g_cfg.backup_root);
        open_folder(g_cfg.backup_root.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Open the exports folder")) {
        saves::ensure_folder(g_cfg.export_root);
        open_folder(g_cfg.export_root.c_str());
    }

    /* ---- import ---- */
    ImGui::Separator();
    ImGui::TextUnformatted("Import a .zip");
    hint("An exported .zip comes back as a backup in the list above; Restore then makes it the saves. "
         "Put the .zip in the exports folder and it shows up here, or type where it is.");
    for (const std::string& a : g_archives) {
        ImGui::PushID(a.c_str());
        if (ImGui::Button("Import")) do_import = a;
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(file_name(a).c_str());
        ImGui::PopID();
    }
    if (!do_import.empty()) run([&] { return saves::import_archive(g_cfg, do_import); });
    ImGui::SetNextItemWidth(-fs * 6.5f);
    const bool entered = ImGui::InputTextWithHint("##zip", "path of a .zip", g_path, sizeof g_path, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_path[0]);
    const bool pressed = ImGui::Button("Import##path");
    ImGui::EndDisabled();
    if ((entered || pressed) && g_path[0]) {
        /* A path pasted with quotes or spaces around it. */
        std::string p = g_path;
        while (!p.empty() && (p.back() == ' ' || p.back() == '"' || p.back() == '\'')) p.pop_back();
        while (!p.empty() && (p[0] == ' ' || p[0] == '"' || p[0] == '\'')) p.erase(0, 1);
        run([&] { return saves::import_archive(g_cfg, p); });
        if (g_note_ok) g_path[0] = 0;
    }

    /* ---- the options ---- */
    ImGui::Separator();
    bool b = s.backup_on_play;
    if (ImGui::Checkbox("Back up the saves each time Play is pressed", &b)) { s.backup_on_play = b; save(); }
    int keep = s.keep_backups;
    ImGui::SetNextItemWidth(fs * 12.0f);
    if (ImGui::SliderInt("Automatic backups to keep", &keep, 1, 30, "%d", ImGuiSliderFlags_AlwaysClamp)) { s.keep_backups = keep; save(); }
    hint("Play makes a backup only when the saves changed since the last one. Beyond this number, the oldest "
         "backups made by Play or before a restore are removed at the next Play; manual and imported ones stay "
         "until you delete them.");

    confirm_popup(running);
}

} /* namespace pui */

#endif
