/* MotorStorm Apocalypse port, UI: display and picture options (ui_internal.h). */
#ifdef PS3RECOMP_UI

#include "ui_internal.h"

#include "imgui.h"
#include "rsx_ui.h"

#include <SDL2/SDL.h>

#include <cstdio>

extern "C" {
void rsx_vulkan_backend_set_fullscreen(int on);
int  rsx_vulkan_backend_fullscreen(void);
void rsx_vulkan_backend_set_vsync(int on);
void rsx_vulkan_backend_set_stretch(int on);
void rsx_vulkan_backend_set_window_size(int w, int h);
void rsx_vulkan_backend_window_pixels(int* w, int* h);
void rsx_vulkan_backend_set_filter(int nearest);
void rsx_vulkan_backend_set_sharpen(int percent);
void rsx_vulkan_backend_set_brightness(int steps);
void rsx_vulkan_backend_set_gamma(int hundredths);
int  rsx_vulkan_backend_picture_pass(void);
}

namespace pui {

namespace {

struct win_size { int w, h; const char* name; };
const win_size k_sizes[] = {
    { 0, 0, "Same as the resolution" },
    { 1280, 720, "1280 x 720" },
    { 1600, 900, "1600 x 900" },
    { 1920, 1080, "1920 x 1080" },
    { 2560, 1440, "2560 x 1440" },
    { 3840, 2160, "3840 x 2160" },
    { -1, -1, "Fit the screen" },
};
const int k_nsizes = (int)(sizeof k_sizes / sizeof k_sizes[0]);

const char* const k_res[] = { "1280 x 720 (as the PS3)", "2560 x 1440", "3840 x 2160 (4K)", "5120 x 2880 (5K)" };

/* A slider that applies while it is dragged and saves when it is let go. */
bool slider(const char* label, int* v, int lo, int hi, const char* fmt)
{
    const bool changed = ImGui::SliderInt(label, v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemDeactivatedAfterEdit()) save();
    return changed;
}

void display(port_settings& s)
{
    ImGui::SeparatorText("Display");
    /* The window as it is: F11 changes it without the menu. */
    int mode = rsx_vulkan_backend_fullscreen() ? 1 : 0;
    static const char* const k_modes[] = { "Window", "Fullscreen (borderless)" };
    if (ImGui::Combo("Display mode  (F11)", &mode, k_modes, 2)) {
        s.fullscreen = mode; rsx_vulkan_backend_set_fullscreen(mode); save();
    }

    int cur = -1;
    for (int i = 0; i < k_nsizes; i++)
        if (k_sizes[i].w == s.window_w && k_sizes[i].h == s.window_h) cur = i;
    if (s.window_w < 0) cur = k_nsizes - 1;
    char custom[48];
    snprintf(custom, sizeof custom, "%d x %d", s.window_w, s.window_h);
    pin_begin(PORT_PIN_WINDOW_SIZE);
    if (ImGui::BeginCombo("Window size", cur >= 0 ? k_sizes[cur].name : custom)) {
        for (int i = 0; i < k_nsizes; i++) {
            if (ImGui::Selectable(k_sizes[i].name, i == cur)) {
                s.window_w = k_sizes[i].w; s.window_h = k_sizes[i].h;
                rsx_vulkan_backend_set_window_size(s.window_w, s.window_h);
                save();
            }
            if (i == cur) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    pin_end(PORT_PIN_WINDOW_SIZE);
    {
        int pw = 0, ph = 0;
        rsx_vulkan_backend_window_pixels(&pw, &ph);
        char t[200];
        snprintf(t, sizeof t, "Now %d x %d pixels. A size larger than the screen is fitted to it.", pw, ph);
        hint(t);
    }

    const int nd = SDL_GetNumVideoDisplays();
    if (nd > 1) {
        const char* name = SDL_GetDisplayName(s.display < nd ? s.display : 0);
        pin_begin(PORT_PIN_DISPLAY);
        if (ImGui::BeginCombo("Monitor", name ? name : "?")) {
            for (int i = 0; i < nd; i++) {
                const char* n = SDL_GetDisplayName(i);
                char label[160]; snprintf(label, sizeof label, "%d: %s", i + 1, n ? n : "?");
                if (ImGui::Selectable(label, i == s.display)) { s.display = i; save(); }
            }
            ImGui::EndCombo();
        }
        pin_end(PORT_PIN_DISPLAY);
    }

    int shape = s.stretch ? 1 : 0;
    static const char* const k_shapes[] = { "16:9 with black bars", "Stretched to the window" };
    if (ImGui::Combo("Picture shape", &shape, k_shapes, 2)) {
        s.stretch = shape; rsx_vulkan_backend_set_stretch(shape); save();
    }
    bool b = s.vsync;
    if (ImGui::Checkbox("VSync", &b)) { s.vsync = b; rsx_vulkan_backend_set_vsync(b); save(); }
    hint("Off, the newest frame is shown without waiting for the screen.");
}

void picture(port_settings& s)
{
    ImGui::SeparatorText("Picture");
    int sc = s.scale >= 1 && s.scale <= 4 ? s.scale - 1 : 0;
    pin_begin(PORT_PIN_SCALE);
    if (ImGui::Combo("Resolution", &sc, k_res, 4)) { s.scale = sc + 1; save(); }
    pin_end(PORT_PIN_SCALE);

    int f = s.filter ? 1 : 0;
    static const char* const k_filters[] = { "Smooth (bilinear)", "Sharp pixels (nearest)" };
    pin_begin(PORT_PIN_FILTER);
    if (ImGui::Combo("Scaling filter", &f, k_filters, 2)) {
        s.filter = f; rsx_vulkan_backend_set_filter(f); save();
    }
    pin_end(PORT_PIN_FILTER);
    hint("Used when the resolution and the window's size differ.");

    const bool pass = rsx_vulkan_backend_picture_pass() != 0;
    ImGui::BeginDisabled(!pass);
    int v = s.sharpen;
    pin_begin(PORT_PIN_SHARPEN);
    if (slider("Sharpening", &v, 0, 100, v ? "%d%%" : "Off")) { s.sharpen = v; rsx_vulkan_backend_set_sharpen(v); }
    pin_end(PORT_PIN_SHARPEN);
    v = s.brightness;
    pin_begin(PORT_PIN_BRIGHTNESS);
    if (slider("Brightness", &v, -50, 50, "%+d")) { s.brightness = v; rsx_vulkan_backend_set_brightness(v); }
    pin_end(PORT_PIN_BRIGHTNESS);
    v = s.gamma;
    char g[16];
    snprintf(g, sizeof g, "%.2f", v / 100.0);
    pin_begin(PORT_PIN_GAMMA);
    if (slider("Gamma", &v, 50, 200, g)) { s.gamma = v; rsx_vulkan_backend_set_gamma(v); }
    pin_end(PORT_PIN_GAMMA);
    const unsigned pins = PORT_PIN_SHARPEN | PORT_PIN_BRIGHTNESS | PORT_PIN_GAMMA;
    ImGui::BeginDisabled((s.sharpen == 0 && s.brightness == 0 && s.gamma == 100) ||
                         (port_settings_pinned() & pins) == pins);
    if (ImGui::Button("Reset these three")) {
        if (!pinned(PORT_PIN_SHARPEN))    { s.sharpen = 0; rsx_vulkan_backend_set_sharpen(0); }
        if (!pinned(PORT_PIN_BRIGHTNESS)) { s.brightness = 0; rsx_vulkan_backend_set_brightness(0); }
        if (!pinned(PORT_PIN_GAMMA))      { s.gamma = 100; rsx_vulkan_backend_set_gamma(100); }
        save();
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    hint(pass ? "Gamma above 1.00 lightens the dark parts. Ctrl+click a slider to type a value."
              : "Sharpening, brightness and gamma are not available on this graphics device.");
}

void quality(port_settings& s)
{
    ImGui::SeparatorText("Quality");
    static const char* const k_aniso_names[] = { "As the game asks", "2x", "4x", "8x", "16x" };
    static const int k_aniso[] = { 0, 2, 4, 8, 16 };
    int ai = 0;
    for (int i = 0; i < 5; i++) if (k_aniso[i] <= s.aniso) ai = i;
    pin_begin(PORT_PIN_ANISO);
    if (ImGui::Combo("Anisotropic filtering", &ai, k_aniso_names, 5)) { s.aniso = k_aniso[ai]; save(); }
    pin_end(PORT_PIN_ANISO);
    hint("Sharper ground and walls at a distance and at an angle.");
    bool b = s.fullres_particles;
    pin_begin(PORT_PIN_PARTICLES);
    if (ImGui::Checkbox("Full-resolution particles", &b)) { s.fullres_particles = b; save(); }
    pin_end(PORT_PIN_PARTICLES);
    hint("Particle effects at the picture's resolution instead of a reduced one.");
}

} /* namespace */

void page_image(port_settings& s)
{
    /* The widgets take what the longest label leaves, so no label is cut
     * off in a narrow page. */
    ImGui::PushItemWidth(-(ImGui::CalcTextSize("Anisotropic filtering").x + ImGui::GetStyle().ItemInnerSpacing.x +
                           ImGui::GetStyle().WindowPadding.x * 0.5f));
    display(s);
    picture(s);
    quality(s);
    ImGui::PopItemWidth();
    const int nd = SDL_GetNumVideoDisplays();
    hint(nd > 1 ? "Resolution, monitor, anisotropic filtering and particles take effect the next time the game starts."
                : "Resolution, anisotropic filtering and particles take effect the next time the game starts.");
}

} /* namespace pui */

#endif
