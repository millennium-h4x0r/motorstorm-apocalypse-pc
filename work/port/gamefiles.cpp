/* MotorStorm Apocalypse port: where the game is and what is there -- see gamefiles.h. */
#include "gamefiles.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <system_error>

namespace fs = std::filesystem;

namespace gamefiles {

const char* const k_eboot_sha256 = "387c9fb795e1709bd2cf1a1145c0548a0fc911a295aa860a27b41441a15418ff";
const unsigned long long k_eboot_size = 21318304ull;
const char* const k_title_id = "BCES00484";
/* USRDIR/ms3.psarc of that disc. */
static const unsigned long long k_psarc_size = 3315000640ull;
/* The cache the game builds in /dev_hdd1 on its first start is about 1.4 GB. */
static const unsigned long long k_cache_need = 2048ull << 20;
static const unsigned long long k_saves_need = 64ull << 20;

/* ---- SHA-256 (FIPS 180-4) --------------------------------------------------- */
namespace {

struct Sha256 {
    uint32_t h[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
    unsigned char buf[64];
    size_t fill = 0;
    uint64_t total = 0;

    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void block(const unsigned char* p)
    {
        static const uint32_t k[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
        };
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
        for (int i = 16; i < 64; i++) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void add(const unsigned char* p, size_t n)
    {
        total += n;
        if (fill) {
            const size_t take = n < 64 - fill ? n : 64 - fill;
            memcpy(buf + fill, p, take);
            fill += take; p += take; n -= take;
            if (fill < 64) return;
            block(buf); fill = 0;
        }
        for (; n >= 64; p += 64, n -= 64) block(p);
        if (n) { memcpy(buf, p, n); fill = n; }
    }

    std::string hex()
    {
        const uint64_t bits = total * 8;
        const unsigned char one = 0x80, zero = 0;
        add(&one, 1);
        while (fill != 56) add(&zero, 1);
        unsigned char len[8];
        for (int i = 0; i < 8; i++) len[i] = (unsigned char)(bits >> (56 - i * 8));
        add(len, 8);
        char out[65];
        for (int i = 0; i < 8; i++) snprintf(out + i * 8, 9, "%08x", h[i]);
        return out;
    }
};

bool is_sep(char c)
{
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

std::string join(const std::string& a, const char* b)
{
    if (a.empty()) return b;
    return is_sep(a.back()) ? a + b : a + "/" + b;
}

/* The last component and what is before it ("" when there is nothing before). */
std::string base_of(const std::string& p)
{
    size_t k = p.size();
    while (k > 0 && !is_sep(p[k - 1])) k--;
    return p.substr(k);
}
std::string parent_of(const std::string& p)
{
    size_t k = p.size();
    while (k > 0 && !is_sep(p[k - 1])) k--;
    if (k == 0) return ".";
    while (k > 1 && is_sep(p[k - 1])) k--;      /* "/x" -> "/" */
    return p.substr(0, k);
}

bool same_name(const std::string& a, const char* b)
{
    if (a.size() != strlen(b)) return false;
    for (size_t i = 0; i < a.size(); i++) {
        const char x = a[i], y = b[i];
        if ((x >= 'a' && x <= 'z' ? x - 32 : x) != (y >= 'a' && y <= 'z' ? y - 32 : y)) return false;
    }
    return true;
}

bool is_dir(const std::string& p) { std::error_code ec; return fs::is_directory(fs::path(p), ec); }
bool is_file(const std::string& p) { std::error_code ec; return fs::is_regular_file(fs::path(p), ec); }
bool exists(const std::string& p) { std::error_code ec; return fs::exists(fs::path(p), ec); }
long long size_of(const std::string& p)
{
    std::error_code ec;
    const auto n = fs::file_size(fs::path(p), ec);
    return ec ? -1 : (long long)n;
}

std::string size_text(unsigned long long n)
{
    char b[48];
    if (n >= 1ull << 30) snprintf(b, sizeof b, "%.1f GB", (double)n / (double)(1ull << 30));
    else if (n >= 1ull << 20) snprintf(b, sizeof b, "%.0f MB", (double)n / (double)(1ull << 20));
    else snprintf(b, sizeof b, "%llu bytes", n);
    return b;
}

/* The first bytes of a file. */
size_t head_of(const std::string& p, unsigned char* out, size_t n)
{
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return 0;
    const size_t got = fread(out, 1, n, f);
    fclose(f);
    return got;
}

/* PARAM.SFO: the string values a check wants. False when it is not an SFO. */
bool read_sfo(const std::string& path, std::map<std::string, std::string>* out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<unsigned char> d(64 * 1024);
    d.resize(fread(d.data(), 1, d.size(), f));
    fclose(f);
    auto u32 = [&](size_t o) { return (uint32_t)d[o] | (uint32_t)d[o + 1] << 8 | (uint32_t)d[o + 2] << 16 |
                                      (uint32_t)d[o + 3] << 24; };
    if (d.size() < 20 || memcmp(d.data(), "\0PSF", 4) != 0) return false;
    const size_t keys = u32(8), data = u32(12), n = u32(16);
    for (size_t i = 0; i < n && 20 + (i + 1) * 16 <= d.size(); i++) {
        const size_t e = 20 + i * 16;
        const size_t ko = keys + ((size_t)d[e] | (size_t)d[e + 1] << 8);
        const unsigned fmt = (unsigned)d[e + 2] | (unsigned)d[e + 3] << 8;
        const size_t len = u32(e + 4), off = data + u32(e + 12);
        if (ko >= d.size() || off >= d.size() || len > d.size() - off) continue;
        if (fmt != 0x0204 && fmt != 0x0004) continue;       /* text only */
        std::string k, v;
        for (size_t p = ko; p < d.size() && d[p]; p++) k += (char)d[p];
        for (size_t p = off; p < off + len && d[p]; p++) v += (char)d[p];
        (*out)[k] = v;
    }
    return true;
}

/* Free bytes on the disk `dir` is (or will be) on: its nearest folder that exists. */
long long free_at(std::string dir)
{
    for (int up = 0; up < 64; up++) {
        std::error_code ec;
        if (fs::exists(fs::path(dir), ec)) {
            const fs::space_info si = fs::space(fs::path(dir), ec);
            return ec ? -1 : (long long)si.available;
        }
        const std::string p = parent_of(dir);
        if (p == dir) break;
        dir = p;
    }
    return -1;
}

bool dir_empty(const std::string& dir)
{
    std::error_code ec;
    fs::directory_iterator it(fs::path(dir), ec);
    return ec || it == fs::directory_iterator();
}

Item item(const char* id, const char* name, State st, const std::string& detail, const std::string& help = "")
{
    Item i;
    i.id = id; i.name = name; i.state = st; i.detail = detail; i.help = help;
    return i;
}

const char* const k_rebuild =
    "The game's code is built into this port from the EBOOT.elf of BCES-00484 (Europe), version 01.00, "
    "and runs only with that one. Another region or version needs the port built again from it "
    "(scripts/lift.sh, then scripts/build.sh).";

Boot g_boot;

} /* namespace */

std::string sha256_file(const std::string& path)
{
    /* The same file again (Check again, the page opened twice) is not read again. */
    static std::map<std::string, std::string> seen;
    std::error_code ec;
    const auto when = fs::last_write_time(fs::path(path), ec);
    char key_tail[64];
    snprintf(key_tail, sizeof key_tail, "|%lld|%lld", size_of(path),
             ec ? 0ll : (long long)when.time_since_epoch().count());
    const std::string key = path + key_tail;
    const auto hit = seen.find(key);
    if (hit != seen.end()) return hit->second;

    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return std::string();
    Sha256 s;
    std::vector<unsigned char> buf(1 << 20);
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) s.add(buf.data(), n);
    const bool bad = ferror(f) != 0;
    fclose(f);
    if (bad) return std::string();
    return seen[key] = s.hex();
}

const Item* Report::find(const char* id) const
{
    for (const Item& i : items)
        if (i.id == id) return &i;
    return nullptr;
}

std::string clean_path(const std::string& typed)
{
    std::string p = typed;
    auto strip = [&]() {
        const size_t a = p.find_first_not_of(" \t\r\n"), b = p.find_last_not_of(" \t\r\n");
        p = a == std::string::npos ? std::string() : p.substr(a, b - a + 1);
    };
    strip();
    if (p.size() >= 2 && (p.front() == '"' || p.front() == '\'') && p.back() == p.front()) {
        p = p.substr(1, p.size() - 2);
        strip();
    }
    if (p.compare(0, 7, "file://") == 0) {
        /* As a file manager hands a folder over: percent-encoded. */
        std::string u;
        for (size_t i = 7; i < p.size(); i++) {
            if (p[i] == '%' && i + 2 < p.size() && isxdigit((unsigned char)p[i + 1]) && isxdigit((unsigned char)p[i + 2])) {
                u += (char)strtol(p.substr(i + 1, 2).c_str(), nullptr, 16);
                i += 2;
            } else u += p[i];
        }
        p = u;
    }
    if (!p.empty() && p[0] == '~' && (p.size() == 1 || is_sep(p[1]))) {
#ifdef _WIN32
        const char* home = getenv("USERPROFILE");
#else
        const char* home = getenv("HOME");
#endif
        if (home && *home) p = std::string(home) + p.substr(1);
    }
    while (p.size() > 1 && is_sep(p.back())
#ifdef _WIN32
           && !(p.size() == 3 && p[1] == ':')      /* C:\ */
#endif
          ) p.pop_back();
    return p;
}

std::string root_of(const std::string& path)
{
    std::string p = path;
    while (p.size() > 1 && is_sep(p.back())) p.pop_back();
    if (p.empty()) return p;
    /* A file, or a name that says file (an EBOOT.elf that is not there yet):
     * the folder it is in. */
    const std::string leaf = base_of(p);
    if (is_file(p) || (!is_dir(p) && (same_name(leaf, "EBOOT.elf") || same_name(leaf, "EBOOT.BIN"))))
        p = parent_of(p);
    if (is_dir(join(p, "PS3_GAME"))) return p;
    const std::string name = base_of(p);
    if (same_name(name, "USRDIR")) return parent_of(parent_of(p));
    if (same_name(name, "PS3_GAME") || is_dir(join(p, "USRDIR"))) return parent_of(p);
    /* A USRDIR by what is in it, whatever it is called. */
    if (exists(join(p, "EBOOT.elf")) || exists(join(p, "EBOOT.BIN")) || exists(join(p, "ms3.psarc")))
        return parent_of(parent_of(p));
    return p;
}

Where where_from_env()
{
    Where w;
    const char* e = getenv("PS3_DEV_FLASH");
    if (e && *e) w.dev_flash = e;
#ifndef _WIN32
    else if (const char* home = getenv("HOME")) {
        /* As main.cpp: RPCS3's install of the firmware, Flatpak or native. */
        static const char* const rel[] = { ".var/app/net.rpcs3.RPCS3/config/rpcs3/dev_flash",
                                           ".config/rpcs3/dev_flash" };
        for (const char* r : rel) {
            const std::string p = join(home, r);
            if (is_dir(p)) { w.dev_flash = p; break; }
        }
    }
#endif
    e = getenv("PS3_SAVEDATA_ROOT");
    w.saves = e && *e ? e : "gamedata/dev_hdd0/home/00000001/savedata";
    e = getenv("PS3_HDD1_ROOT");
    w.cache = e && *e ? e : "hdd1";
    return w;
}

Report check(const std::string& path, const Where& where)
{
    Report r;
    r.given = path;
    r.root = root_of(path);
    if (r.root.empty()) r.root = ".";
    const std::string game = join(r.root, "PS3_GAME");
    r.usrdir = join(game, "USRDIR");
    r.eboot = join(r.usrdir, "EBOOT.elf");

    /* 1. the folder */
    const bool have_dir = is_dir(r.usrdir);
    if (have_dir) {
        r.items.push_back(item("folder", "The game's folder", FOUND, r.usrdir));
    } else {
        r.items.push_back(item("folder", "The game's folder", MISSING,
            exists(r.root) ? "No PS3_GAME/USRDIR folder in " + r.root : r.root + " does not exist",
            "Choose the folder of your own MotorStorm Apocalypse disc dump (BCES-00484, Europe): the one "
            "that holds PS3_GAME. PS3_GAME itself, its USRDIR folder or the EBOOT.elf in it work too."));
    }
    const char* const after_folder = "Looked for once the game's folder is found";

    /* 2. PARAM.SFO: which game this is */
    {
        const std::string sfo = join(game, "PARAM.SFO");
        std::map<std::string, std::string> v;
        if (!have_dir) {
            r.items.push_back(item("sfo", "The disc (PARAM.SFO)", MISSING, after_folder));
        } else if (!exists(sfo)) {
            r.items.push_back(item("sfo", "The disc (PARAM.SFO)", MISSING, "No PARAM.SFO in " + game,
                "PARAM.SFO names the game and its saves. It is in the PS3_GAME folder of the dump: copy that "
                "folder whole, not only USRDIR."));
        } else if (!read_sfo(sfo, &v) || v["TITLE_ID"].empty()) {
            r.items.push_back(item("sfo", "The disc (PARAM.SFO)", WRONG, "PARAM.SFO cannot be read",
                "Copy PARAM.SFO again from the PS3_GAME folder of the dump."));
        } else if (v["TITLE_ID"] != k_title_id) {
            r.items.push_back(item("sfo", "The disc (PARAM.SFO)", WRONG,
                "This is " + v["TITLE_ID"] + (v["TITLE"].empty() ? "" : " (" + v["TITLE"] + ")") +
                ", not BCES-00484", k_rebuild));
        } else {
            r.items.push_back(item("sfo", "The disc (PARAM.SFO)", FOUND,
                "BCES-00484" + (v["TITLE"].empty() ? std::string() : ", " + v["TITLE"])));
        }
    }

    /* 3. EBOOT.elf: there, an ELF, and the one the port was lifted from */
    {
        const char* const name = "The game's program (EBOOT.elf)";
        const std::string bin = join(r.usrdir, "EBOOT.BIN");
        const std::string steps =
            "To decrypt it with RPCS3 (rpcs3.net):\n"
            "  1. Install RPCS3 and, in it, the PS3 system software (File > Install Firmware).\n"
            "  2. In RPCS3: Utilities > Decrypt PS3 Binaries.\n"
            "  3. Pick EBOOT.BIN from the game's PS3_GAME/USRDIR folder.\n"
            "  4. RPCS3 writes EBOOT.elf beside it. Keep it in that folder, then Check again.";
        unsigned char head[4] = { 0, 0, 0, 0 };
        if (!have_dir) {
            r.items.push_back(item("eboot", name, MISSING, after_folder));
        } else if (!exists(r.eboot)) {
            r.items.push_back(item("eboot", name, MISSING,
                exists(bin) ? "EBOOT.BIN is here, still encrypted; EBOOT.elf is not"
                            : std::string("Neither EBOOT.elf nor EBOOT.BIN is in the game's USRDIR folder"),
                "The disc has the game's program as EBOOT.BIN, which is encrypted. " + steps));
        } else if (head_of(r.eboot, head, 4) != 4 || memcmp(head, "\x7f" "ELF", 4) != 0) {
            const bool sce = memcmp(head, "SCE\0", 4) == 0;
            r.items.push_back(item("eboot", name, WRONG,
                sce ? "This EBOOT.elf is still encrypted (an EBOOT.BIN under another name)"
                    : "This EBOOT.elf is not a program file",
                "Renaming EBOOT.BIN does not unlock it. " + steps));
        } else {
            const long long size = size_of(r.eboot);
            const std::string sha = sha256_file(r.eboot);
            if (sha == k_eboot_sha256) {
                r.items.push_back(item("eboot", name, FOUND, "BCES-00484 version 01.00, decrypted"));
            } else if (sha.empty()) {
                r.items.push_back(item("eboot", name, WRONG, "EBOOT.elf cannot be read",
                    "Check that the file can be read by you, or copy it again."));
            } else {
                char d[200];
                snprintf(d, sizeof d, "Not the EBOOT.elf of BCES-00484 version 01.00 (%lld bytes, sha256 %.12s...)",
                         size, sha.c_str());
                r.items.push_back(item("eboot", name, WRONG, d,
                    std::string(k_rebuild) + " Decrypt the EBOOT.BIN of that disc itself, not of an update "
                    "(1.01, 1.02)."));
            }
        }
    }

    /* 4. ms3.psarc: nearly all of the game's data */
    {
        const char* const name = "The game's data (ms3.psarc)";
        const std::string psarc = join(r.usrdir, "ms3.psarc");
        const char* const whole = "ms3.psarc (3.1 GB) holds the tracks, the vehicles and the sound. Copy the "
                                  "whole PS3_GAME folder of the dump again.";
        unsigned char head[4] = { 0, 0, 0, 0 };
        const long long size = have_dir ? size_of(psarc) : -1;
        if (!have_dir) {
            r.items.push_back(item("data", name, MISSING, after_folder));
        } else if (!exists(psarc)) {
            r.items.push_back(item("data", name, MISSING, "No ms3.psarc in " + r.usrdir, whole));
        } else if (head_of(psarc, head, 4) != 4 || memcmp(head, "PSAR", 4) != 0) {
            r.items.push_back(item("data", name, WRONG, "ms3.psarc is not a PSARC archive", whole));
        } else if (size >= 0 && (unsigned long long)size < k_psarc_size) {
            r.items.push_back(item("data", name, WRONG,
                "ms3.psarc is incomplete: " + size_text((unsigned long long)size) + " of " + size_text(k_psarc_size),
                whole));
        } else if ((unsigned long long)size != k_psarc_size) {
            r.items.push_back(item("data", name, WARNING,
                "ms3.psarc is " + size_text((unsigned long long)size) + ", the disc's is " + size_text(k_psarc_size),
                "It is not the file of the BCES-00484 disc. The game may stop while loading."));
        } else {
            r.items.push_back(item("data", name, FOUND, size_text((unsigned long long)size)));
        }
    }

    /* 5. what the game opens outside the archive */
    {
        const char* const name = "Videos, fonts and budgets (ui/, budgets/)";
        static const char* const need[] = { "ui/video/evologo_60.avi", "ui/video/maintitles_60.avi", "ui/fonts",
                                            "budgets/budgets.xml" };
        std::string lost;
        int nlost = 0;
        for (const char* n : need)
            if (!exists(join(r.usrdir, n))) { lost += nlost++ ? ", " : ""; lost += n; }
        if (!have_dir) r.items.push_back(item("media", name, MISSING, after_folder));
        else if (nlost) r.items.push_back(item("media", name, MISSING, "Not in USRDIR: " + lost,
            "The ui and budgets folders are beside ms3.psarc in the dump's USRDIR. Copy the whole PS3_GAME "
            "folder of the dump again."));
        else r.items.push_back(item("media", name, FOUND, "The intro videos, ui/fonts and budgets.xml"));
    }

    /* 6. the PS3 system software: the game reads its MP3 decoder and fonts */
    {
        const char* const name = "PS3 system software (firmware)";
#ifdef _WIN32
        const char* const how = "Install the official PS3 system software in RPCS3 (File > Install Firmware, "
                                "with PS3UPDAT.PUP from Sony's site), then copy RPCS3's dev_flash folder beside "
                                "the game's folder, or set PS3_DEV_FLASH to it.";
#else
        const char* const how = "Install the official PS3 system software in RPCS3 (File > Install Firmware, "
                                "with PS3UPDAT.PUP from Sony's site). The port reads it from RPCS3's own folder; "
                                "elsewhere, set PS3_DEV_FLASH to the dev_flash folder.";
#endif
        std::string flash = where.dev_flash;
#ifdef _WIN32
        if (flash.empty() && is_dir(join(parent_of(r.root), "dev_flash"))) flash = join(parent_of(r.root), "dev_flash");
#endif
        if (flash.empty() || !is_dir(flash)) {
            r.items.push_back(item("firmware", name, WARNING,
                flash.empty() ? "Not found: races have no sound and some text has no font"
                              : flash + " does not exist: races have no sound and some text has no font", how));
        } else if (!exists(join(flash, "sys/external/flashMP3.pic"))) {
            r.items.push_back(item("firmware", name, WARNING,
                "No sys/external/flashMP3.pic in " + flash + ": races have no sound", how));
        } else {
            r.items.push_back(item("firmware", name, FOUND, flash));
        }
    }

    /* 7. room for the saves and, before the first start, the cache */
    {
        const char* const name = "Free space";
        const long long saves = free_at(where.saves), cache = free_at(where.cache);
        const bool cache_built = is_dir(where.cache) && !dir_empty(where.cache);
        if (saves >= 0 && (unsigned long long)saves < k_saves_need) {
            r.items.push_back(item("space", name, WARNING,
                "Only " + size_text((unsigned long long)saves) + " free where the saves go (" + where.saves + ")",
                "The game cannot save when the disk is full. Free some space."));
        } else if (!cache_built && cache >= 0 && (unsigned long long)cache < k_cache_need) {
            r.items.push_back(item("space", name, WARNING,
                "Only " + size_text((unsigned long long)cache) + " free for the game's cache (" + where.cache + ")",
                "On its first start the game builds a cache of about 1.4 GB. Free some space."));
        } else if (saves < 0) {
            r.items.push_back(item("space", name, WARNING, "Cannot tell how much is free at " + where.saves));
        } else {
            r.items.push_back(item("space", name, FOUND,
                size_text((unsigned long long)saves) + " free where the saves go"));
        }
    }

    r.ready = true;
    for (const Item& i : r.items) {
        if (i.state != MISSING && i.state != WRONG) continue;
        if (r.ready) {
            /* One line for the start screen. */
            if (i.id == "folder") r.problem = "The game's folder is not set";
            else if (i.id == "eboot") r.problem = i.state == MISSING ? "EBOOT.elf is missing"
                                                                    : "EBOOT.elf is not the right one";
            else if (i.id == "sfo") r.problem = i.state == MISSING ? "PARAM.SFO is missing" : "This is another game or region";
            else if (i.id == "data") r.problem = i.state == MISSING ? "ms3.psarc is missing" : "ms3.psarc is damaged";
            else r.problem = "Some of the game's files are missing";
        }
        r.ready = false;
    }
    return r;
}

const Boot& resolve(int argc, char** argv, const char* settings_path)
{
    Boot b;
    const char* arg = nullptr;
    for (int i = 1; i < argc && !arg; i++)
        if (argv[i][0] && strncmp(argv[i], "--", 2) != 0) arg = argv[i];
    const char* env = getenv("PS3_GAME_DIR");
    if (arg && exists(arg)) { b.source = FROM_ARGUMENT; b.given = arg; }
    else if (env && *env) { b.source = FROM_ENVIRONMENT; b.given = clean_path(env); }
    else if (settings_path && *settings_path) { b.source = FROM_SETTINGS; b.given = clean_path(settings_path); }
    else { b.source = FROM_DEFAULT; b.given = "disc"; }
    if (arg && b.source != FROM_ARGUMENT)
        fprintf(stderr, "[game] %s does not exist: looking in %s\n", arg, b.given.c_str());
    b.report = check(b.given, where_from_env());
    g_boot = b;
    return g_boot;
}

const Boot& boot() { return g_boot; }

} /* namespace gamefiles */
