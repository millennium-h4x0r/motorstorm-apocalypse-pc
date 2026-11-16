/* saves.cpp: list, back up, skip when unchanged, prune, restore, delete,
 * export and import round trips, and archives that must be refused. All on a
 * scratch folder with made-up saves.
 *   c++ -std=c++17 -g -fsanitize=address,undefined -I.. ../saves.cpp saves_test.cpp -o /tmp/svt && /tmp/svt */
#include "saves.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

typedef std::vector<uint8_t> Bytes;

static void put16(Bytes& v, uint32_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }
static void put32(Bytes& v, uint32_t x) { put16(v, x & 0xFFFF); put16(v, x >> 16); }

static uint32_t crc32(const Bytes& d)
{
    uint32_t c = 0xFFFFFFFFu;
    for (uint8_t b : d) {
        c ^= b;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return c ^ 0xFFFFFFFFu;
}

static void write(const fs::path& p, const Bytes& d)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write((const char*)d.data(), (std::streamsize)d.size());
}
static Bytes read(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static Bytes text(const std::string& s) { return Bytes(s.begin(), s.end()); }

/* A PARAM.SFO with TITLE, SUB_TITLE and DETAIL (the layout in saves.cpp). */
static Bytes make_sfo(const std::string& title, const std::string& sub, const std::string& detail)
{
    const char* keys[] = { "DETAIL", "SUB_TITLE", "TITLE" };
    const std::string* vals[] = { &detail, &sub, &title };
    const uint32_t maxs[] = { 1024, 128, 128 };
    Bytes b = { 0, 'P', 'S', 'F', 1, 1, 0, 0 };
    const uint32_t ktab = 0x14 + 16 * 3;
    uint32_t klen = 0;
    for (const char* k : keys) klen += (uint32_t)strlen(k) + 1;
    const uint32_t dtab = ktab + ((klen + 3) & ~3u);
    put32(b, ktab); put32(b, dtab); put32(b, 3);
    uint32_t ko = 0, dof = 0;
    for (int i = 0; i < 3; i++) {
        put16(b, ko); put16(b, 0x0204); put32(b, (uint32_t)vals[i]->size() + 1); put32(b, maxs[i]); put32(b, dof);
        ko += (uint32_t)strlen(keys[i]) + 1; dof += maxs[i];
    }
    for (const char* k : keys) { b.insert(b.end(), k, k + strlen(k) + 1); }
    b.resize(dtab + dof, 0);
    dof = 0;
    for (int i = 0; i < 3; i++) { memcpy(&b[dtab + dof], vals[i]->data(), vals[i]->size()); dof += maxs[i]; }
    return b;
}

/* Every file under a folder, by relative name. */
static std::map<std::string, Bytes> tree(const fs::path& root)
{
    std::map<std::string, Bytes> m;
    if (!fs::exists(root)) return m;
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it)
        if (it->is_regular_file() && !it->is_symlink()) m[fs::relative(it->path(), root).generic_string()] = read(it->path());
    return m;
}

/* Every name under a folder, hidden ones too: what a refused import must leave as it was. */
static std::vector<std::string> names(const fs::path& root)
{
    std::vector<std::string> v;
    if (!fs::exists(root)) return v;
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it)
        v.push_back(fs::relative(it->path(), root).generic_string());
    std::sort(v.begin(), v.end());
    return v;
}

/* A .zip written by hand, so that it can be wrong in chosen ways. */
struct ZFile {
    std::string name;
    Bytes data;                 /* the bytes in the archive */
    uint32_t method = 0;
    uint32_t usize = 0, crc = 0;
    uint32_t made = 3 << 8 | 20, eattr = 0100644u << 16, flags = 0;
    std::string local_name;     /* when not the same */
    bool has_local_name = false;
};
static ZFile zf(const std::string& name, const Bytes& d)
{
    ZFile f;
    f.name = name; f.data = d; f.usize = (uint32_t)d.size(); f.crc = crc32(d);
    return f;
}
static Bytes make_zip(const std::vector<ZFile>& files)
{
    Bytes z, dir;
    for (const ZFile& f : files) {
        const std::string ln = f.has_local_name ? f.local_name : f.name;
        const uint32_t at = (uint32_t)z.size();
        put32(z, 0x04034b50); put16(z, 20); put16(z, f.flags); put16(z, f.method); put16(z, 0); put16(z, 0x21);
        put32(z, f.crc); put32(z, (uint32_t)f.data.size()); put32(z, f.usize);
        put16(z, (uint32_t)ln.size()); put16(z, 0);
        z.insert(z.end(), ln.begin(), ln.end());
        z.insert(z.end(), f.data.begin(), f.data.end());
        put32(dir, 0x02014b50); put16(dir, f.made); put16(dir, 20); put16(dir, f.flags); put16(dir, f.method);
        put16(dir, 0); put16(dir, 0x21); put32(dir, f.crc); put32(dir, (uint32_t)f.data.size()); put32(dir, f.usize);
        put16(dir, (uint32_t)f.name.size()); put16(dir, 0); put16(dir, 0); put16(dir, 0); put16(dir, 0);
        put32(dir, f.eattr); put32(dir, at);
        dir.insert(dir.end(), f.name.begin(), f.name.end());
    }
    const uint32_t cd = (uint32_t)z.size();
    z.insert(z.end(), dir.begin(), dir.end());
    put32(z, 0x06054b50); put16(z, 0); put16(z, 0); put16(z, (uint32_t)files.size()); put16(z, (uint32_t)files.size());
    put32(z, (uint32_t)dir.size()); put32(z, cd); put16(z, 0);
    return z;
}

/* Raw deflate streams made once with zlib (level 9): a dynamic-Huffman block
 * of ghost_text(), and a fixed-Huffman block of "abcabcabcabc-fixed". */
static const uint8_t k_dyn[] = {
    237,213,77,78,195,48,20,69,225,173,60,91,42,204,32,255,69,133,205,148,196,73,156,182,73,90,63,71,42,20,214,142,218,201,157,92,118,144,5,124,58,195,19,174,163,246,78,125,45,221,48,5,149,227,126,150,77,82,52,18,92,173,211,69,212,159,156,36,233,46,203,95,138,109,37,55,9,15,16,239,160,7,200,31,96,1,120,191,131,18,224,81,232,207,0,142,21,202,55,128,129,21,244,12,144,179,194,20,1,12,43,84,22,160,97,133,73,1,10,86,8,11,128,178,130,252,2,24,86,184,88,128,87,86,80,11,208,178,194,205,2,244,172,32,11,64,201,10,50,2,52,172,32,17,192,179,130,30,0,42,86,56,214,0,145,21,194,55,192,204,10,126,2,216,178,194,254,2,48,179,194,117,6,80,86,56,117,0,134,21,102,11,16,88,97,92,0,34,43,56,11,240,195,10,242,12,96,88,65,7,0,195,10,146,1,24,86,216,100,0,145,21,250,14,224,196,10,73,14,160,172,144,148,0,3,43,184,5,224,147,21,210,15,128,47,86,200,91,128,145,21,244,0,112,102,133,93,1,48,177,66,99,1,90,86,240,14,192,176,66,86,2,68,86,144,8,96,88,161,182,0,79,172,144,39,0,158,21,66,7,144,178,130,120,128,148,21,94,42,128,150,21,156,3,200,88,97,29,202,58,148,117,40,235,80,214,161,252,51,148,63 };
static const uint8_t k_fix[] = { 75,76,74,78,132,33,221,180,204,138,212,20,0 };
static Bytes ghost_text()
{
    std::string s;
    for (int i = 0; i < 60; i++) s += "synthetic ghost lap %03d sector time 01:23.456 | ";
    Bytes b(s.begin(), s.end());
    for (size_t i = 0; i < b.size(); i += 11) b[i] = (uint8_t)((b[i] + (i * 7) % 3) & 0x7f);
    return b;
}
static ZFile zdeflated(const std::string& name, const uint8_t* d, size_t n, const Bytes& plain)
{
    ZFile f;
    f.name = name; f.data.assign(d, d + n); f.method = 8; f.usize = (uint32_t)plain.size(); f.crc = crc32(plain);
    return f;
}

static fs::path g_root;
static saves::Config g_cfg;
static int g_zip_n;

/* Import `z`; it must be refused and leave the backups folder as it was. */
static void refused(const char* what, const Bytes& z)
{
    const fs::path p = g_root / "hostile" / ("h" + std::to_string(g_zip_n++) + ".zip");
    write(p, z);
    const std::vector<std::string> before = names(g_root);
    const saves::Result r = saves::import_archive(g_cfg, p.string());
    if (r.ok) { printf("FAIL accepted: %s\n", what); fails++; }
    if (r.message.empty()) { printf("FAIL no message: %s\n", what); fails++; }
    if (names(g_root) != before) { printf("FAIL left files: %s\n", what); fails++; }
}

int main()
{
    g_root = fs::temp_directory_path() / ("saves_test." + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(g_root);
    const fs::path sroot = g_root / "hdd0" / "savedata", broot = g_root / "user" / "backups", eroot = g_root / "user" / "exports";
    g_cfg.save_root = sroot.string() + "/";      /* a trailing slash must not matter */
    g_cfg.backup_root = broot.string();
    g_cfg.export_root = eroot.string();
    const saves::Config& c = g_cfg;
    const Bytes sfo = make_sfo("TEST GAME (TEST00001)", "User Save\nData", "Progress and ghosts.");

    /* ---- nothing there yet ---- */
    CHECK(saves::list_saves(c.save_root).empty());
    CHECK(saves::list_backups(c).empty());
    CHECK(saves::list_archives(c).empty());
    { saves::Result r = saves::backup(c, "manual"); CHECK(r.ok && r.skipped); }
    { saves::Result r = saves::backup_for_play(c, 8); CHECK(r.ok && r.skipped); }
    CHECK(!fs::exists(broot));                                      /* a skipped backup makes nothing */
    CHECK(!saves::export_archive(c, "").ok);
    CHECK(!saves::restore(c, "20260101-000000", false).ok);
    CHECK(!saves::remove_backup(c, "20260101-000000").ok);
    CHECK(saves::prune(c, 3) == 0);

    /* ---- the list ---- */
    Bytes data(100 * 1024);
    for (size_t i = 0; i < data.size(); i++) data[i] = (uint8_t)(i * 31 + i / 7);
    write(sroot / "TEST00001" / "PARAM.SFO", sfo);
    write(sroot / "TEST00001" / "AUTOSAVE.DAT", data);
    write(sroot / "TEST00001" / "ICON0.PNG", text("not really a picture"));
    write(sroot / "NOSFO" / "DATA.BIN", text("x"));
    write(sroot / "BADSFO" / "PARAM.SFO", text("garbage that is not a PSF at all"));
    {
        Bytes cut = sfo; cut.resize(0x30);                           /* a PSF cut inside its index */
        write(sroot / "CUTSFO" / "PARAM.SFO", cut);
        Bytes wild = sfo; wild[12] = 0xF0; wild[13] = 0xFF; wild[14] = 0xFF; wild[15] = 0x7F;   /* data table far outside */
        write(sroot / "WILDSFO" / "PARAM.SFO", wild);
    }
    {
        const std::vector<saves::SaveInfo> v = saves::list_saves(c.save_root);
        CHECK(v.size() == 5);
        const saves::SaveInfo* t = nullptr;
        for (const auto& s : v) {
            if (s.dir == "TEST00001") t = &s;
            else CHECK(s.title == s.dir);                            /* no readable PARAM.SFO: the folder's name */
        }
        CHECK(t != nullptr);
        if (t) {
            CHECK(t->title == "TEST GAME (TEST00001)" && t->subtitle == "User Save Data" && t->detail == "Progress and ghosts.");
            CHECK(t->files == 3 && t->size == sfo.size() + data.size() + 20);
            CHECK(!t->icon.empty() && fs::exists(t->icon));
            CHECK(t->mtime > 0 && saves::format_time(t->mtime).size() >= 16);
        }
    }
    fs::remove_all(sroot / "BADSFO"); fs::remove_all(sroot / "CUTSFO"); fs::remove_all(sroot / "WILDSFO");
    const std::map<std::string, Bytes> v1 = tree(sroot);
    CHECK(v1.size() == 4);

    /* ---- back up: manual always, Play only when something changed ---- */
    const auto t0 = std::chrono::steady_clock::now();
    const saves::Result m1 = saves::backup(c, "manual");
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(m1.ok && !m1.skipped && tree(broot / m1.path) == v1);
    CHECK(m1.path.size() >= 22 && m1.path.compare(15, 7, "-manual") == 0);
    const saves::Result m2 = saves::backup(c, "manual");               /* the same second: another name */
    CHECK(m2.ok && !m2.skipped && m2.path != m1.path && tree(broot / m2.path) == v1);
    { saves::Result r = saves::backup_for_play(c, 8); CHECK(r.ok && r.skipped); }
    CHECK(saves::list_backups(c).size() == 2);
    data[5] ^= 1;                                                       /* one bit, the same size */
    write(sroot / "TEST00001" / "AUTOSAVE.DAT", data);
    const std::map<std::string, Bytes> v2 = tree(sroot);
    const saves::Result p1 = saves::backup_for_play(c, 8);
    CHECK(p1.ok && !p1.skipped && tree(broot / p1.path) == v2);
    { saves::Result r = saves::backup_for_play(c, 8); CHECK(r.ok && r.skipped); }
    {
        const std::vector<saves::BackupInfo> v = saves::list_backups(c);
        CHECK(v.size() == 3);
        if (v.size() == 3) {
            CHECK(v[0].name == p1.path && v[0].reason == "play" && v[0].automatic && v[0].saves == 2);
            CHECK(v[1].name == m2.path && v[2].name == m1.path && !v[1].automatic && v[1].reason == "manual");
            CHECK(v[0].size == sfo.size() + data.size() + 20 + 1);
        }
    }
    for (const auto& n : names(broot)) CHECK(n.find(".tmp-") == std::string::npos);
#ifndef _WIN32
    /* A symbolic link among the saves is not followed and not copied. */
    write(g_root / "outside" / "secret.txt", text("not a save"));
    CHECK(symlink((g_root / "outside" / "secret.txt").c_str(), (sroot / "TEST00001" / "LINK.DAT").c_str()) == 0);
    CHECK(symlink((g_root / "outside").c_str(), (sroot / "LINKDIR").c_str()) == 0);
    {
        const saves::Result r = saves::backup(c, "manual");
        CHECK(r.ok && tree(broot / r.path) == v2);
        CHECK(!fs::exists(fs::symlink_status(broot / r.path / "TEST00001" / "LINK.DAT")));
        CHECK(!fs::exists(fs::symlink_status(broot / r.path / "LINKDIR")));
        const saves::Result e = saves::export_archive(c, "");
        CHECK(e.ok);
        const Bytes z = read(e.path);
        const std::string zs(z.begin(), z.end());
        CHECK(zs.find("LINK") == std::string::npos && zs.find("secret") == std::string::npos && zs.find("not a save") == std::string::npos);
        fs::remove(e.path);
        CHECK(saves::remove_backup(c, r.path).ok && !fs::exists(broot / r.path));
    }
    fs::remove(sroot / "TEST00001" / "LINK.DAT"); fs::remove(sroot / "LINKDIR");
#endif

    /* ---- restore ---- */
    write(sroot / "EXTRA" / "PARAM.SFO", sfo);                          /* a save the backup does not have */
    const std::map<std::string, Bytes> v3 = tree(sroot);
    {
        saves::Result r = saves::restore(c, m1.path, true);              /* the game runs: refused, nothing moves */
        CHECK(!r.ok && tree(sroot) == v3 && saves::list_backups(c).size() == 3);
        for (const char* bad : { "", ".", "..", "../backups", ".tmp-x", "nope", "a/b", "/etc" })
            CHECK(!saves::restore(c, bad, false).ok);
        CHECK(tree(sroot) == v3 && saves::list_backups(c).size() == 3);
        r = saves::restore(c, m1.path, false);
        CHECK(r.ok);
        CHECK(tree(sroot) == v1);                                        /* exactly the backup: EXTRA is gone */
        CHECK(!fs::exists(sroot / "EXTRA"));
        const std::vector<saves::BackupInfo> v = saves::list_backups(c);
        CHECK(v.size() == 4 && v[0].reason == "restore" && v[0].automatic && v[0].name == r.path);
        if (!v.empty()) CHECK(tree(broot / v[0].name) == v3);            /* the saves as they were */
        CHECK(tree(broot / m1.path) == v1);                              /* the backup itself is untouched */
        for (const auto& n : names(g_root / "hdd0")) CHECK(n.find(".restore-") == std::string::npos);
        /* back again, from the "before restore" one */
        r = saves::restore(c, v[0].name, false);
        CHECK(r.ok && tree(sroot) == v3);
        /* with no saves folder at all (a fresh install, saves brought from elsewhere) */
        fs::remove_all(g_root / "hdd0");
        r = saves::restore(c, m1.path, false);
        CHECK(r.ok && r.path.empty() && tree(sroot) == v1);
    }

    /* ---- delete ---- */
    {
        const size_t n = saves::list_backups(c).size();
        for (const char* bad : { "", "..", "../exports", ".tmp-x", "nope" }) CHECK(!saves::remove_backup(c, bad).ok);
        CHECK(saves::list_backups(c).size() == n && fs::exists(g_root / "user"));
        CHECK(saves::remove_backup(c, m2.path).ok);
        CHECK(saves::list_backups(c).size() == n - 1 && !fs::exists(broot / m2.path));
    }

    /* ---- prune: automatic ones beyond `keep`, never the others ---- */
    {
        fs::remove_all(broot);
        const char* fake[] = {
            "20250101-100000", "20250102-100000",                        /* the port's old names: manual */
            "20250103-100000-manual", "20250104-100000-import",
            "20250105-100000-play", "20250106-100000-play", "20250107-100000-restore", "20250108-100000-play",
            "20250109-100000-play", "20250110-100000-play-2", "saves-20250111-100000-play", "renamed by hand",
        };
        for (const char* f : fake) write(broot / f / "TEST00001" / "PARAM.SFO", sfo);
        write(broot / ".tmp-20250112-100000-play" / "TEST00001" / "PARAM.SFO", sfo);     /* a crash's leftover: not listed */
        write(broot / "a file.txt", text("x"));
        std::vector<saves::BackupInfo> v = saves::list_backups(c);
        CHECK(v.size() == 12);
        if (v.size() == 12) {
            CHECK(v[0].name == "renamed by hand" && v[0].reason == "manual" && !v[0].automatic);   /* its folder's time: now */
            CHECK(v[1].name == "saves-20250111-100000-play" && v[1].reason == "play");
            CHECK(v[2].name == "20250110-100000-play-2" && v[2].reason == "play");
            CHECK(v[11].name == "20250101-100000" && v[11].reason == "manual" && !v[11].automatic);
            CHECK(saves::format_time(v[11].time) == "1 Jan 2025 10:00");
            CHECK(v[5].name == "20250107-100000-restore" && v[5].automatic);
        }
        CHECK(saves::prune(c, 3) == 4);
        v = saves::list_backups(c);
        CHECK(v.size() == 8);
        int autos = 0;
        for (const auto& b : v) autos += b.automatic;
        CHECK(autos == 3);
        for (const char* keep : { "20250101-100000", "20250102-100000", "20250103-100000-manual", "20250104-100000-import",
                                  "renamed by hand", "saves-20250111-100000-play", "20250110-100000-play-2", "20250109-100000-play" })
            CHECK(fs::exists(broot / keep));
        CHECK(fs::exists(broot / "a file.txt"));
        CHECK(saves::prune(c, 3) == 0);
        CHECK(saves::prune(c, 0) == 2);                                  /* at least one stays */
        CHECK(fs::exists(broot / "saves-20250111-100000-play"));
        /* an old-style backup restores */
        const saves::Result r = saves::restore(c, "20250101-100000", false);
        CHECK(r.ok && tree(sroot).size() == 1);
        fs::remove_all(broot);
        fs::remove_all(sroot);
        for (const auto& kv : v3) write(sroot / kv.first, kv.second);
    }

    /* ---- export, import ---- */
    {
        const saves::Result b = saves::backup(c, "manual");
        const saves::Result e1 = saves::export_archive(c, "");            /* the saves as they are */
        const saves::Result e2 = saves::export_archive(c, b.path);        /* a backup */
        CHECK(e1.ok && e2.ok && e1.path != e2.path && fs::exists(e1.path) && fs::exists(e2.path));
        CHECK(e2.path.find("-manual.zip") != std::string::npos);
        CHECK(!saves::export_archive(c, "nope").ok && !saves::export_archive(c, "../backups").ok);
        CHECK(saves::list_archives(c).size() == 2);
        for (const auto& n : names(eroot)) CHECK(n.find(".tmp-") == std::string::npos);
#ifndef _WIN32
        if (system("command -v unzip >/dev/null 2>&1") == 0) {            /* another unzip reads it */
            CHECK(system(("unzip -tqq '" + e1.path + "' >/dev/null 2>&1").c_str()) == 0);
            CHECK(system(("mkdir '" + (g_root / "unz").string() + "' && cd '" + (g_root / "unz").string() +
                          "' && unzip -qq '" + e1.path + "'").c_str()) == 0);
            CHECK(tree(g_root / "unz") == v3);
        } else printf("saves_test: no unzip here, that check is skipped\n");
#endif
        fs::remove_all(broot);
        const saves::Result i1 = saves::import_archive(c, e1.path);
        CHECK(i1.ok && tree(broot / i1.path) == v3);
        const std::vector<saves::BackupInfo> v = saves::list_backups(c);
        CHECK(v.size() == 1 && v[0].reason == "import" && !v[0].automatic && v[0].saves == 3);
        CHECK(tree(sroot) == v3);                                          /* an import does not touch the saves */
        /* and the imported one restores */
        fs::remove_all(sroot);
        CHECK(saves::restore(c, i1.path, false).ok && tree(sroot) == v3);
        CHECK(!saves::import_archive(c, (g_root / "no such.zip").string()).ok);
        CHECK(!saves::import_archive(c, sroot.string()).ok);              /* a folder */

        /* deflated entries, both block kinds, and one folder around the saves */
        const Bytes ghost = ghost_text();
        Bytes z = make_zip({ zf("Saves/", {}), zf("Saves/TEST00001/PARAM.SFO", sfo),
                             zdeflated("Saves/TEST00001/GHOST.DAT", k_dyn, sizeof k_dyn, ghost),
                             zdeflated("Saves/TEST00001/SMALL.DAT", k_fix, sizeof k_fix, text("abcabcabcabc-fixed")),
                             zf("Saves/TEST00001/EMPTY.DAT", {}) });
        write(g_root / "in" / "deflated.zip", z);
        const saves::Result i2 = saves::import_archive(c, (g_root / "in" / "deflated.zip").string());
        CHECK(i2.ok);
        if (i2.ok) {
            const auto t = tree(broot / i2.path);
            CHECK(t.size() == 4 && t.count("TEST00001/GHOST.DAT") && t.at("TEST00001/GHOST.DAT") == ghost);
            CHECK(t.count("TEST00001/SMALL.DAT") && t.at("TEST00001/SMALL.DAT") == text("abcabcabcabc-fixed"));
            CHECK(t.count("TEST00001/EMPTY.DAT") && t.at("TEST00001/EMPTY.DAT").empty());
        }
#ifndef _WIN32
        if (system("command -v zip >/dev/null 2>&1") == 0) {              /* a .zip another tool made */
            const std::string out = (g_root / "in" / "tool.zip").string();
            CHECK(system(("cd '" + sroot.string() + "' && zip -q -r -9 '" + out + "' .").c_str()) == 0);
            const saves::Result i3 = saves::import_archive(c, out);
            CHECK(i3.ok && tree(broot / i3.path) == v3);
        } else printf("saves_test: no zip here, that check is skipped\n");
#endif
    }

    /* ---- archives that must be refused, whole ---- */
    {
        const Bytes ghost = ghost_text();
        const ZFile good = zf("TEST00001/PARAM.SFO", sfo);
        auto with = [&](ZFile f) { return make_zip({ good, f }); };
        refused("empty file", {});
        refused("not a zip", text("this is a text file, not an archive, and long enough to hold an end record"));
        refused("no entries", make_zip({}));
        refused("parent path", with(zf("../evil.txt", text("x"))));
        refused("parent path inside", with(zf("TEST00001/../../evil.txt", text("x"))));
        refused("parent as the save", with(zf("../PARAM.SFO", sfo)));
        refused("absolute path", with(zf("/tmp/evil/PARAM.SFO", sfo)));
        refused("drive path", with(zf("C:/evil/PARAM.SFO", sfo)));
        refused("backslashes", with(zf("TEST00001\\..\\..\\evil.txt", text("x"))));
        refused("unc path", with(zf("//server/share/x", text("x"))));
        refused("dot folder", with(zf("./TEST00001/DATA.BIN", text("x"))));
        refused("hidden file", with(zf("TEST00001/.hidden", text("x"))));
        refused("empty component", with(zf("TEST00001//DATA.BIN", text("x"))));
        refused("device name", with(zf("TEST00001/NUL", text("x"))));
        refused("device name with an extension", with(zf("TEST00001/com1.txt", text("x"))));
        refused("trailing dot", with(zf("TEST00001/DATA.", text("x"))));
        refused("control character", with(zf("TEST00001/DA\nTA", text("x"))));
        refused("NUL in a name", with(zf(std::string("TEST00001/DA\0TA", 14), text("x"))));
        refused("other characters", with(zf("TEST00001/a|b", text("x"))));
        refused("long name", with(zf("TEST00001/" + std::string(200, 'A'), text("x"))));
        refused("file outside a save", with(zf("README.txt", text("x"))));
        refused("folders inside a save", with(zf("TEST00001/sub/DATA.BIN", text("x"))));
        refused("too deep", with(zf("a/b/c/d/DATA.BIN", text("x"))));
        refused("the same file twice", with(zf("TEST00001/PARAM.SFO", sfo)));
        refused("the same file twice, by case", with(zf("test00001/param.sfo", sfo)));
        refused("no PARAM.SFO", make_zip({ zf("TEST00001/DATA.BIN", text("x")) }));
        refused("PARAM.SFO that is not one", make_zip({ zf("TEST00001/PARAM.SFO", text("hello")) }));
        refused("wrapped, two wrappers", make_zip({ zf("A/TEST00001/PARAM.SFO", sfo), zf("B/TEST00001/PARAM.SFO", sfo) }));
        { ZFile f = zf("TEST00001/LINK", text("/etc/passwd")); f.eattr = 0120777u << 16; refused("symbolic link", with(f)); }
        { ZFile f = zf("TEST00001/FIFO", {}); f.eattr = 0010644u << 16; refused("special file", with(f)); }
        { ZFile f = zf("TEST00001/DATA.BIN", text("x")); f.flags = 1; refused("encrypted", with(f)); }
        { ZFile f = zf("TEST00001/DATA.BIN", text("x")); f.method = 12; refused("another compression", with(f)); }
        { ZFile f = zf("TEST00001/DATA.BIN", text("x")); f.crc ^= 1; refused("wrong CRC, stored", with(f)); }
        { ZFile f = zdeflated("TEST00001/GHOST.DAT", k_dyn, sizeof k_dyn, ghost); f.crc ^= 1; refused("wrong CRC, deflated", with(f)); }
        { ZFile f = zf("TEST00001/DATA.BIN", text("xyz")); f.usize = 2; refused("stored, sizes differ", with(f)); }
        { ZFile f = zf("TEST00001/DATA.BIN", text("x")); f.has_local_name = true; f.local_name = "TEST00001/OTHER.BI";
          refused("local name differs", with(f)); }
        { ZFile f = zf("TEST00001/DATA.BIN", text("x")); f.has_local_name = true; f.local_name = "../../../../evil";
          refused("local name escapes", with(f)); }
        /* deflate that lies about what it unpacks to */
        { ZFile f = zdeflated("TEST00001/GHOST.DAT", k_dyn, sizeof k_dyn, ghost); f.usize -= 100; refused("unpacks to more than it says", with(f)); }
        { ZFile f = zdeflated("TEST00001/GHOST.DAT", k_dyn, sizeof k_dyn, ghost); f.usize += 100; refused("unpacks to less than it says", with(f)); }
        { ZFile f = zdeflated("TEST00001/GHOST.DAT", k_dyn, sizeof k_dyn / 2, ghost); refused("deflate cut short", with(f)); }
        { ZFile f = zdeflated("TEST00001/BOMB.DAT", k_fix, sizeof k_fix, text("abcabcabcabc-fixed")); f.usize = 0xFFFFFFF0u;
          refused("says it unpacks to 4 GB", with(f)); }
        { ZFile f = zdeflated("TEST00001/BOMB.DAT", k_fix, sizeof k_fix, text("abcabcabcabc-fixed")); f.usize = 0xFFFFFFFFu;
          refused("zip64 size", with(f)); }
        { std::vector<ZFile> many = { good };
          for (int i = 0; i < 2100; i++) many.push_back(zf("TEST00001/F" + std::to_string(i), {}));
          refused("too many files", make_zip(many)); }
        { std::vector<ZFile> big = { good };                             /* each within bounds, together over the limit */
          ZFile f = zdeflated("x", k_fix, sizeof k_fix, text("abcabcabcabc-fixed")); f.usize = 40u << 20;
          for (int i = 0; i < 4; i++) { f.name = "TEST00001/BIG" + std::to_string(i); big.push_back(f); }
          refused("unpacks to more than the limit", make_zip(big)); }
        /* a damaged directory */
        { Bytes z = with(zf("TEST00001/DATA.BIN", text("x"))); z.resize(z.size() - 10); refused("cut at the end", z); }
        { Bytes z = with(zf("TEST00001/DATA.BIN", text("x"))); z[z.size() - 6] = 0xFF; z[z.size() - 5] = 0xFF; z[z.size() - 4] = 0xFF;
          refused("directory offset outside", z); }
        { Bytes z = with(zf("TEST00001/DATA.BIN", text("x"))); z[z.size() - 12] = 9; refused("entry count wrong", z); }
        { Bytes z = with(zf("TEST00001/DATA.BIN", text("x"))); z[z.size() - 18] = 1; refused("several parts", z); }
        {   /* the first entry's local header offset points into the directory */
            Bytes z = make_zip({ good });
            const size_t cd = z.size() - 22 - (46 + good.name.size());
            z[cd + 42] = (uint8_t)cd; z[cd + 43] = (uint8_t)(cd >> 8);
            refused("local header inside the directory", z);
        }
        { Bytes z(saves::k_max_archive + 1, 0); refused("larger than the limit", z); }

        /* A valid archive with bytes changed at random: refused or taken, never a crash, and never
         * anything outside the backups folder. (The sanitizers watch the reads.) */
        const Bytes base = make_zip({ zf("Saves/TEST00001/PARAM.SFO", sfo),
                                      zdeflated("Saves/TEST00001/GHOST.DAT", k_dyn, sizeof k_dyn, ghost),
                                      zdeflated("Saves/TEST00001/SMALL.DAT", k_fix, sizeof k_fix, text("abcabcabcabc-fixed")) });
        const std::vector<std::string> outside = names(g_root / "hdd0");
        uint32_t rng = 12345;
        auto next = [&]() { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
        const fs::path fz = g_root / "in" / "fuzz.zip";
        int taken = 0;
        /* the tail (directory and end record) and the deflated data get most of the changes */
        const size_t sfo_end = 30 + strlen("Saves/TEST00001/PARAM.SFO") + sfo.size();
        for (int i = 0; i < 3000; i++) {
            Bytes z = base;
            const int nflip = 1 + (int)(next() % 3);
            for (int k = 0; k < nflip; k++) {
                const size_t at = sfo_end + next() % (z.size() - sfo_end);
                z[at] = (i & 1) ? (uint8_t)next() : (uint8_t)(z[at] ^ (1u << (next() % 8)));
            }
            if (i % 50 == 0) z.resize(sfo_end + next() % (z.size() - sfo_end));
            write(fz, z);
            const saves::Result r = saves::import_archive(c, fz.string());
            if (r.ok) { taken++; saves::remove_backup(c, r.path); }
        }
        CHECK(names(g_root / "hdd0") == outside);
        for (const auto& n : names(broot)) CHECK(n.find(".tmp-") == std::string::npos && n.find("evil") == std::string::npos);
        printf("saves_test: 3000 damaged archives, %d still valid and taken\n", taken);
    }

    /* ---- words ---- */
    CHECK(saves::format_size(812) == "812 bytes" && saves::format_size(104 * 1024) == "104 KB");
    CHECK(saves::format_size(1258291) == "1.2 MB");
    CHECK(!strcmp(saves::reason_label("play"), "before Play") && !strcmp(saves::reason_label("x"), "manual"));

    fs::remove_all(g_root);
    printf("saves_test: a manual backup of a 100 KB save took %.1f ms\n", ms);
    printf(fails ? "saves_test: %d FAILED\n" : "saves_test: ok\n", fails);
    return fails != 0;
}
