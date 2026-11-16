/* gamefiles.cpp: the path forms, SHA-256, and each check on made-up trees.
 *   c++ -std=c++20 -I.. ../gamefiles.cpp gamefiles_test.cpp -o /tmp/gt && /tmp/gt [real EBOOT.elf]
 * With the path of the real EBOOT.elf, also that it is recognised. */
#include "gamefiles.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace gamefiles;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void put(const std::string& path, const std::string& bytes)
{
    fs::create_directories(fs::path(path).parent_path());
    FILE* f = fopen(path.c_str(), "wb");
    fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
}

/* A PARAM.SFO with TITLE_ID, TITLE and VERSION. */
static std::string sfo(const char* id, const char* title)
{
    const char* keys[] = { "TITLE", "TITLE_ID", "VERSION" };
    std::string vals[] = { title, id, "01.00" };
    std::string kt, dt, idx;
    auto le = [](uint32_t v, int n) { std::string s; for (int i = 0; i < n; i++) s += (char)(v >> (8 * i)); return s; };
    for (int i = 0; i < 3; i++) {
        idx += le((uint32_t)kt.size(), 2) + le(0x0204, 2) + le((uint32_t)vals[i].size() + 1, 4) +
               le((uint32_t)vals[i].size() + 1, 4) + le((uint32_t)dt.size(), 4);
        kt += keys[i]; kt += '\0';
        dt += vals[i]; dt += '\0';
    }
    const uint32_t ks = 20 + 16 * 3, ds = ks + (uint32_t)kt.size();
    return std::string("\0PSF", 4) + le(0x101, 4) + le(ks, 4) + le(ds, 4) + le(3, 4) + idx + kt + dt;
}

static State st(const Report& r, const char* id) { const Item* i = r.find(id); return i ? i->state : FOUND; }

int main(int argc, char** argv)
{
    char dir[] = "/tmp/gamefiles_test.XXXXXX";
    if (!mkdtemp(dir)) return 2;
    const std::string d = dir;

    /* SHA-256: the standard's vectors */
    put(d + "/abc", "abc");
    CHECK(sha256_file(d + "/abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    put(d + "/empty", "");
    CHECK(sha256_file(d + "/empty") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    put(d + "/two", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
    CHECK(sha256_file(d + "/two") == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    put(d + "/million", std::string(1000000, 'a'));
    CHECK(sha256_file(d + "/million") == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    CHECK(sha256_file(d + "/nope").empty());

    /* a path as typed */
    const std::string real_home = getenv("HOME") ? getenv("HOME") : "";
    setenv("HOME", "/home/x", 1);
    CHECK(clean_path("  /a/b c/  ") == "/a/b c");
    CHECK(clean_path("\"/a/b c\"") == "/a/b c");
    CHECK(clean_path("'/a/b'") == "/a/b");
    CHECK(clean_path("file:///a/b%20c/PS3_GAME") == "/a/b c/PS3_GAME");
    CHECK(clean_path("~/dump") == "/home/x/dump");
    CHECK(clean_path("~") == "/home/x");
    CHECK(clean_path("/") == "/");
    CHECK(clean_path("").empty());

    /* the four forms, with spaces in the path */
    const std::string root = d + "/my dump/BCES 00484";
    const std::string usr = root + "/PS3_GAME/USRDIR";
    fs::create_directories(usr);
    CHECK(root_of(root) == root);
    CHECK(root_of(root + "/") == root);
    CHECK(root_of(root + "/PS3_GAME") == root);
    CHECK(root_of(usr) == root);
    CHECK(root_of(usr + "/") == root);
    CHECK(root_of(usr + "/EBOOT.elf") == root);            /* not there yet */
    put(usr + "/EBOOT.elf", "x");
    CHECK(root_of(usr + "/EBOOT.elf") == root);
    CHECK(root_of("disc/PS3_GAME/USRDIR/EBOOT.elf") == "disc");
    CHECK(root_of("PS3_GAME/USRDIR/EBOOT.elf") == ".");
    CHECK(root_of(d + "/nothing here") == d + "/nothing here");
    /* a USRDIR under another name, known by what is in it */
    put(d + "/odd/game/files/ms3.psarc", "PSAR");
    CHECK(root_of(d + "/odd/game/files") == d + "/odd");
    fs::remove(usr + "/EBOOT.elf");

    Where w;
    w.saves = d + "/saves/a/b";            /* not there yet: its nearest folder counts */
    w.cache = d + "/hdd1";
    w.dev_flash = d + "/flash";

    /* nothing at all */
    Report r = check(d + "/nothing here", w);
    CHECK(!r.ready && st(r, "folder") == MISSING && st(r, "eboot") == MISSING && st(r, "data") == MISSING);
    CHECK(r.problem == "The game's folder is not set");
    CHECK(st(r, "firmware") == WARNING);
    CHECK(r.find("space") != nullptr);

    /* an empty USRDIR */
    r = check(usr, w);
    CHECK(r.root == root && r.usrdir == usr && r.eboot == usr + "/EBOOT.elf");
    CHECK(!r.ready && st(r, "folder") == FOUND && st(r, "sfo") == MISSING && st(r, "eboot") == MISSING);

    /* the data, the right PARAM.SFO; EBOOT.BIN only */
    put(root + "/PS3_GAME/PARAM.SFO", sfo("BCES00484", "MotorStorm Apocalypse"));
    put(usr + "/ui/video/evologo_60.avi", "x");
    put(usr + "/ui/video/maintitles_60.avi", "x");
    put(usr + "/ui/fonts/f", "x");
    put(usr + "/budgets/budgets.xml", "x");
    put(usr + "/EBOOT.BIN", std::string("SCE\0", 4) + "rest");
    r = check(root, w);
    CHECK(st(r, "sfo") == FOUND && r.find("sfo")->detail == "BCES-00484, MotorStorm Apocalypse");
    CHECK(st(r, "media") == FOUND);
    CHECK(st(r, "eboot") == MISSING && r.find("eboot")->detail.find("EBOOT.BIN is here") != std::string::npos);
    CHECK(r.find("eboot")->help.find("Decrypt PS3 Binaries") != std::string::npos);
    CHECK(r.problem == "EBOOT.elf is missing");
    CHECK(st(r, "data") == MISSING);

    /* EBOOT.BIN renamed; then some other ELF */
    fs::copy_file(usr + "/EBOOT.BIN", usr + "/EBOOT.elf");
    r = check(root, w);
    CHECK(st(r, "eboot") == WRONG && r.find("eboot")->detail.find("still encrypted") != std::string::npos);
    put(usr + "/EBOOT.elf", std::string("\x7f" "ELF", 4) + std::string(100, 'z'));
    r = check(root + "/PS3_GAME", w);
    CHECK(st(r, "eboot") == WRONG && r.find("eboot")->detail.find("Not the EBOOT.elf of BCES-00484") != std::string::npos);
    CHECK(r.find("eboot")->help.find("scripts/lift.sh") != std::string::npos);
    CHECK(r.problem == "EBOOT.elf is not the right one");

    /* ms3.psarc: not an archive, then cut short */
    put(usr + "/ms3.psarc", "junk");
    r = check(root, w);
    CHECK(st(r, "data") == WRONG);
    put(usr + "/ms3.psarc", "PSAR and not much more");
    r = check(root, w);
    CHECK(st(r, "data") == WRONG && r.find("data")->detail.find("incomplete") != std::string::npos);

    /* another game */
    put(root + "/PS3_GAME/PARAM.SFO", sfo("BCUS98242", "MotorStorm Apocalypse"));
    r = check(root, w);
    CHECK(st(r, "sfo") == WRONG && r.find("sfo")->detail.find("BCUS98242") != std::string::npos);
    put(root + "/PS3_GAME/PARAM.SFO", "not an sfo");
    r = check(root, w);
    CHECK(st(r, "sfo") == WRONG);

    /* a video gone */
    fs::remove(usr + "/ui/video/evologo_60.avi");
    r = check(root, w);
    CHECK(st(r, "media") == MISSING && r.find("media")->detail.find("evologo_60.avi") != std::string::npos);

    /* the firmware */
    fs::create_directories(d + "/flash");
    r = check(root, w);
    CHECK(st(r, "firmware") == WARNING && r.find("firmware")->detail.find("flashMP3.pic") != std::string::npos);
    put(d + "/flash/sys/external/flashMP3.pic", "x");
    r = check(root, w);
    CHECK(st(r, "firmware") == FOUND);

    /* which rule decides: the argument when it exists, PS3_GAME_DIR, the settings, disc/ */
    {
        const std::string elf = usr + "/EBOOT.elf";
        char a0[] = "bin", play[] = "--play";
        char* av[] = { a0, play, (char*)elf.c_str(), nullptr };
        unsetenv("PS3_GAME_DIR");
        const Boot* b = &resolve(3, av, "/from/settings");
        CHECK(b->source == FROM_ARGUMENT && b->report.root == root);
        const std::string gone = d + "/gone/EBOOT.elf";
        char* av2[] = { a0, (char*)gone.c_str(), nullptr };
        b = &resolve(2, av2, " ~/from settings/ ");
        CHECK(b->source == FROM_SETTINGS && b->given == "/home/x/from settings");
        setenv("PS3_GAME_DIR", usr.c_str(), 1);
        b = &resolve(2, av2, "/from/settings");
        CHECK(b->source == FROM_ENVIRONMENT && b->report.root == root);
        unsetenv("PS3_GAME_DIR");
        char* av3[] = { a0, play, nullptr };
        b = &resolve(2, av3, "");
        CHECK(b->source == FROM_DEFAULT && b->given == "disc");
        b = &resolve(1, av3, nullptr);
        CHECK(b->source == FROM_DEFAULT && &boot() == b);
    }

    /* the real one, when given */
    if (argc > 1) {
        setenv("HOME", real_home.c_str(), 1);
        CHECK(sha256_file(argv[1]) == k_eboot_sha256);
        r = check(argv[1], where_from_env());
        for (const Item& i : r.items)
            printf("  %-8s %d  %s\n", i.id.c_str(), (int)i.state, i.detail.c_str());
        CHECK(st(r, "eboot") == FOUND);
    }

    fs::remove_all(d);
    printf(fails ? "%d FAILED\n" : "gamefiles_test: all passed\n", fails);
    return fails ? 1 : 0;
}
