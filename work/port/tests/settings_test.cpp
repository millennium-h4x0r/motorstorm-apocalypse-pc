/* settings.cpp: round trip, defaults, bad values, the environment winning,
 * pinned fields, the restart environment.
 *   c++ -std=c++17 -I.. ../settings.cpp settings_test.cpp -o /tmp/st && /tmp/st */
#include "settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main()
{
    char dir[] = "/tmp/settings_test.XXXXXX";
    if (!mkdtemp(dir)) return 2;
    const std::string path = std::string(dir) + "/sub/settings.ini";

    port_settings s, r;
    CHECK(port_settings_load(&s, path.c_str()) == 0);          /* missing: defaults */
    CHECK(s.scale == 1 && s.record_thread == 1 && !strcmp(s.voice, "english") && !strcmp(s.text, "english"));

    s.scale = 3; s.fullscreen = 1; s.vsync = 1; s.stretch = 1; s.display = 2;
    snprintf(s.voice, sizeof s.voice, "german"); snprintf(s.text, sizeof s.text, "french");
    s.fps_limit = 60; s.fps_counter = 1; s.record_thread = 0; s.skip_launcher = 1;
    CHECK(port_settings_save(&s, path.c_str()) == 0);            /* creates sub/ */
    CHECK(port_settings_load(&r, path.c_str()) == 1);
    CHECK(!memcmp(&s, &r, sizeof s));

    /* bad values fall back, unknown keys and sections are ignored */
    FILE* f = fopen(path.c_str(), "w");
    fputs("[video]\nscale = 9\nvsync = yes\n[language]\nvoice = klingon\ntext = french ; c\n"
          "[nope]\nx = 1\ngarbage line\n[performance]\nfps_limit = 120\n", f);
    fclose(f);
    CHECK(port_settings_load(&r, path.c_str()) == 1);
    CHECK(r.scale == 1 && r.vsync == 0 && !strcmp(r.voice, "english") && !strcmp(r.text, "french"));
    CHECK(r.fps_limit == 120);

    /* export: the environment wins; the console language follows the text */
    unsetenv("PS3RECOMP_RES_SCALE"); unsetenv("PS3_SYS_LANG"); setenv("PS3_VOICE_LANG", "japanese", 1);
    r.scale = 2;
    port_settings_export(&r);
    CHECK(!strcmp(getenv("PS3RECOMP_RES_SCALE"), "2"));
    CHECK(!strcmp(getenv("PS3_VOICE_LANG"), "japanese"));
    CHECK(!strcmp(getenv("PS3_TEXT_LANG"), "french"));
    CHECK(!strcmp(getenv("PS3_SYS_LANG"), "fr"));

    /* the console language for other texts; an unknown one is English */
    {
        static const char* const k[][2] = { { "german", "de" }, { "british", "en-gb" }, { "italian", "it" } };
        for (const auto& m : k) {
            port_settings t; port_settings_defaults(&t);
            snprintf(t.text, sizeof t.text, "%s", m[0]);
            unsetenv("PS3_SYS_LANG");
            port_settings_export(&t);
            CHECK(!strcmp(getenv("PS3_SYS_LANG"), m[1]));
        }
        port_settings t; port_settings_defaults(&t);
        snprintf(t.text, sizeof t.text, "klingon");
        unsetenv("PS3_SYS_LANG");
        port_settings_export(&t);
        CHECK(!strcmp(getenv("PS3_SYS_LANG"), "en"));
    }

    /* the environment pins what it sets: the struct shows it, the file keeps its own */
    {
        const std::string p2 = std::string(dir) + "/pin.ini";
        port_settings a; port_settings_defaults(&a);
        a.scale = 1; snprintf(a.voice, sizeof a.voice, "english");
        CHECK(port_settings_save(&a, p2.c_str()) == 0);
        setenv("PS3RECOMP_RES_SCALE", "3", 1); unsetenv("PS3_VOICE_LANG"); unsetenv("PS3_TEXT_LANG");
        unsetenv("PS3RECOMP_VK_FULLSCREEN"); unsetenv("PS3RECOMP_VK_PRESENT"); unsetenv("PS3RECOMP_VK_STRETCH");
        unsetenv("PS3RECOMP_VK_DISPLAY"); unsetenv("PS3RECOMP_FPS_LIMIT"); setenv("PS3RECOMP_BE_THREAD", "0", 1);
        unsetenv("PS3RECOMP_FULLRES_PARTICLES"); unsetenv("PS3RECOMP_ANISO");
        unsetenv("PS3RECOMP_VK_WINDOW_SIZE"); unsetenv("PS3RECOMP_VK_FILTER"); unsetenv("PS3RECOMP_VK_SHARPEN");
        unsetenv("PS3RECOMP_VK_BRIGHTNESS"); unsetenv("PS3RECOMP_VK_GAMMA");
        unsetenv("PS3_SIM_FPS");                              /* the exports above set it */
        port_settings b;
        CHECK(port_settings_load(&b, p2.c_str()) == 1);
        const unsigned pin = port_settings_from_env(&b);
        CHECK(pin == (PORT_PIN_SCALE | PORT_PIN_RECORD_THREAD));
        CHECK(port_settings_pinned() == pin);
        CHECK(b.scale == 3 && b.record_thread == 0 && !strcmp(b.voice, "english"));
        b.fps_counter = 1;                                    /* the UI changes something else */
        CHECK(port_settings_save(&b, p2.c_str()) == 0);
        port_settings c;
        CHECK(port_settings_load(&c, p2.c_str()) == 1);
        CHECK(c.scale == 1 && c.record_thread == 1 && c.fps_counter == 1);
        unsetenv("PS3RECOMP_RES_SCALE"); unsetenv("PS3RECOMP_BE_THREAD");
        port_settings_from_env(&c);                           /* nothing pinned again */
        CHECK(port_settings_pinned() == 0);
        remove(p2.c_str());
    }

    /* mods: saved, read back, clamped */
    {
        const std::string p3 = std::string(dir) + "/mods.ini";
        port_settings a; port_settings_defaults(&a);
        CHECK(a.fullres_particles == 1 && a.aniso == 16);
        a.fullres_particles = 0; a.aniso = 4;
        CHECK(port_settings_save(&a, p3.c_str()) == 0);
        port_settings b;
        CHECK(port_settings_load(&b, p3.c_str()) == 1);
        CHECK(b.fullres_particles == 0 && b.aniso == 4);
        FILE* g = fopen(p3.c_str(), "w");
        fputs("[mods]\naniso = 64\nfullres_particles = 7\n", g);
        fclose(g);
        CHECK(port_settings_load(&b, p3.c_str()) == 1);
        CHECK(b.aniso == 16 && b.fullres_particles == 1);   /* out of range: the defaults */
        remove(p3.c_str());
    }

    /* mod list: toggled in order, read back, exported only when on disk */
    {
        port_settings a; port_settings_defaults(&a);
        port_mod_set(&a, "alpha", 1); port_mod_set(&a, "beta", 1); port_mod_set(&a, "alpha", 0);
        port_mod_set(&a, "alpha", 1);
        CHECK(!strcmp(a.mods_on, "beta,alpha"));
        CHECK(port_mod_enabled(&a, "alpha") && port_mod_enabled(&a, "beta") && !port_mod_enabled(&a, "alp"));
        const std::string p4 = std::string(dir) + "/modlist.ini";
        CHECK(port_settings_save(&a, p4.c_str()) == 0);
        port_settings b;
        CHECK(port_settings_load(&b, p4.c_str()) == 1);
        CHECK(!strcmp(b.mods_on, "beta,alpha"));
        remove(p4.c_str());
    }

    /* display and picture: round trip, bad values, the environment */
    {
        const std::string p5 = std::string(dir) + "/picture.ini";
        unsetenv("PS3RECOMP_VK_WINDOW_SIZE"); unsetenv("PS3RECOMP_VK_FILTER"); unsetenv("PS3RECOMP_VK_SHARPEN");
        unsetenv("PS3RECOMP_VK_BRIGHTNESS"); unsetenv("PS3RECOMP_VK_GAMMA");
        port_settings a; port_settings_defaults(&a);
        { port_settings n; port_settings_defaults(&n); port_settings_from_env(&n); }   /* nothing pinned */
        CHECK(a.window_w == 0 && a.window_h == 0 && a.filter == 0 && a.sharpen == 0 && a.brightness == 0 &&
              a.gamma == 100);
        a.scale = 4; a.window_w = 1920; a.window_h = 1080; a.filter = 1; a.sharpen = 35; a.brightness = -12;
        a.gamma = 140;
        CHECK(port_settings_save(&a, p5.c_str()) == 0);
        port_settings b;
        CHECK(port_settings_load(&b, p5.c_str()) == 1);
        CHECK(!memcmp(&a, &b, sizeof a));
        a.window_w = a.window_h = -1;                             /* fit the screen */
        CHECK(port_settings_save(&a, p5.c_str()) == 0);
        CHECK(port_settings_load(&b, p5.c_str()) == 1);
        CHECK(b.window_w == -1 && b.window_h == -1);
        FILE* g = fopen(p5.c_str(), "w");
        fputs("[video]\nscale = 5\nwindow = 10x10\nfilter = cubic\nsharpen = 101\nbrightness = -51\ngamma = 20\n", g);
        fclose(g);
        CHECK(port_settings_load(&b, p5.c_str()) == 1);          /* out of range: the defaults */
        CHECK(b.scale == 1 && b.window_w == 0 && b.window_h == 0 && b.filter == 0 && b.sharpen == 0 &&
              b.brightness == 0 && b.gamma == 100);
        g = fopen(p5.c_str(), "w");
        fputs("[video]\nwindow = 2560x1440 ; a comment\nfilter = nearest\nbrightness = 50\ngamma = 200\n", g);
        fclose(g);
        CHECK(port_settings_load(&b, p5.c_str()) == 1);
        CHECK(b.window_w == 2560 && b.window_h == 1440 && b.filter == 1 && b.brightness == 50 && b.gamma == 200);

        /* the environment wins, is clamped as the backend clamps it, and the file keeps its own */
        setenv("PS3RECOMP_VK_WINDOW_SIZE", "fit", 1); setenv("PS3RECOMP_VK_SHARPEN", "250", 1);
        setenv("PS3RECOMP_VK_GAMMA", "x", 1); setenv("PS3RECOMP_VK_FILTER", "linear", 1);
        const unsigned pin = port_settings_from_env(&b);
        CHECK((pin & PORT_PIN_WINDOW_SIZE) && (pin & PORT_PIN_SHARPEN) && (pin & PORT_PIN_GAMMA) &&
              (pin & PORT_PIN_FILTER) && !(pin & PORT_PIN_BRIGHTNESS));
        CHECK(b.window_w == -1 && b.sharpen == 100 && b.gamma == 100 && b.filter == 0 && b.brightness == 50);
        b.sharpen = 7; b.brightness = 9;                          /* the UI changes both */
        CHECK(port_settings_save(&b, p5.c_str()) == 0);
        port_settings c;
        CHECK(port_settings_load(&c, p5.c_str()) == 1);
        CHECK(c.window_w == 2560 && c.window_h == 1440 && c.filter == 1 && c.sharpen == 0 && c.gamma == 200);
        CHECK(c.brightness == 9);                                 /* not pinned: saved */
        unsetenv("PS3RECOMP_VK_WINDOW_SIZE"); unsetenv("PS3RECOMP_VK_SHARPEN");
        unsetenv("PS3RECOMP_VK_GAMMA"); unsetenv("PS3RECOMP_VK_FILTER");

        /* exported as the backend reads them */
        unsetenv("PS3RECOMP_VK_BRIGHTNESS");
        setenv("PS3_VOICE_LANG", "english", 1);                   /* the caller's: export must not claim it */
        c.window_w = 1600; c.window_h = 900; c.sharpen = 40;
        port_settings_export(&c);
        CHECK(!strcmp(getenv("PS3RECOMP_VK_WINDOW_SIZE"), "1600x900") && !strcmp(getenv("PS3RECOMP_VK_FILTER"), "nearest"));
        CHECK(!strcmp(getenv("PS3RECOMP_VK_SHARPEN"), "40") && !strcmp(getenv("PS3RECOMP_VK_BRIGHTNESS"), "9") &&
              !strcmp(getenv("PS3RECOMP_VK_GAMMA"), "200"));
        remove(p5.c_str());
    }

    /* voices: text-only languages are refused */
    f = fopen(path.c_str(), "w");
    fputs("\xEF\xBB\xBF[language]\nvoice = korean\ntext = korean\n", f);   /* with a BOM */
    fclose(f);
    CHECK(port_settings_load(&r, path.c_str()) == 1);
    CHECK(!strcmp(r.voice, "english") && !strcmp(r.text, "korean"));

    /* the restart environment: without what export set, with the skip */
    {
        unsetenv("PS3RECOMP_RES_SCALE"); unsetenv("PS3_SKIP_LAUNCHER");
        setenv("SETTINGS_TEST_KEEP", "1", 1); setenv("PS3_VOICE_LANG", "english", 1);
        port_settings e; port_settings_defaults(&e);
        port_settings_export(&e);                             /* RES_SCALE is ours now */
        char** env = port_settings_restart_env("SETTINGS_TEST_EXTRA=1");
        CHECK(env != nullptr);
        bool scale = false, keep = false, skip = false, voice = false, extra = false;
        for (char** p = env; p && *p; p++) {
            if (!strncmp(*p, "PS3RECOMP_RES_SCALE=", 20)) scale = true;
            if (!strcmp(*p, "SETTINGS_TEST_KEEP=1")) keep = true;
            if (!strcmp(*p, "PS3_SKIP_LAUNCHER=1")) skip = true;
            if (!strncmp(*p, "PS3_VOICE_LANG=", 15)) voice = true;   /* the caller's, not ours */
            if (!strcmp(*p, "SETTINGS_TEST_EXTRA=1")) extra = true;
        }
        CHECK(!scale && keep && skip && voice && extra);
        CHECK(getenv("PS3RECOMP_RES_SCALE") != nullptr);      /* this process keeps its own */
        free(env);
    }

    /* [game] path: empty by default; the whole line, with spaces, ; and #; PS3_GAME_DIR pins it */
    {
        const std::string p5 = std::string(dir) + "/game.ini";
        unsetenv("PS3_GAME_DIR");
        port_settings a; port_settings_defaults(&a);
        CHECK(a.game_path[0] == 0);
        snprintf(a.game_path, sizeof a.game_path, "/mnt/My Games/dump #2; europe/BCES-00484");
        CHECK(port_settings_save(&a, p5.c_str()) == 0);
        port_settings b;
        CHECK(port_settings_load(&b, p5.c_str()) == 1);
        CHECK(!strcmp(b.game_path, "/mnt/My Games/dump #2; europe/BCES-00484"));
        CHECK(!(port_settings_from_env(&b) & PORT_PIN_GAME));
        setenv("PS3_GAME_DIR", "/from/env", 1);
        CHECK(port_settings_from_env(&b) & PORT_PIN_GAME);
        CHECK(!strcmp(b.game_path, "/from/env"));              /* what the game runs with */
        CHECK(port_settings_save(&b, p5.c_str()) == 0);        /* the file keeps its own */
        unsetenv("PS3_GAME_DIR");
        CHECK(port_settings_load(&b, p5.c_str()) == 1);
        CHECK(!strcmp(b.game_path, "/mnt/My Games/dump #2; europe/BCES-00484"));
        port_settings_from_env(&b);                            /* nothing pinned for what follows */
        a.game_path[0] = 0;
        CHECK(port_settings_save(&a, p5.c_str()) == 0);
        CHECK(port_settings_load(&b, p5.c_str()) == 1 && b.game_path[0] == 0);
        remove(p5.c_str());
    }

    /* [saves]: on and 8 by default, saved, read back, clamped */
    {
        const std::string p6 = std::string(dir) + "/saves.ini";
        port_settings a; port_settings_defaults(&a);
        CHECK(a.backup_on_play == 1 && a.keep_backups == 8);
        a.backup_on_play = 0; a.keep_backups = 25;
        CHECK(port_settings_save(&a, p6.c_str()) == 0);
        port_settings b;
        CHECK(port_settings_load(&b, p6.c_str()) == 1);
        CHECK(b.backup_on_play == 0 && b.keep_backups == 25);
        FILE* g = fopen(p6.c_str(), "w");
        fputs("[saves]\nbackup_on_play = 2\nkeep_backups = 0\n", g);
        fclose(g);
        CHECK(port_settings_load(&b, p6.c_str()) == 1);
        CHECK(b.backup_on_play == 1 && b.keep_backups == 8);   /* out of range: the defaults */
        remove(p6.c_str());
    }

    remove(path.c_str());
    printf(fails ? "settings_test: %d FAILED\n" : "settings_test: ok\n", fails);
    return fails != 0;
}
